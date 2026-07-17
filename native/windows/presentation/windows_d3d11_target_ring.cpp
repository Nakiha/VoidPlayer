#include "windows/presentation/windows_d3d11_target_ring.h"

#include <algorithm>
#include <unordered_set>

namespace vr {
namespace {

bool supported_target_format(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool same_com_identity(ID3D11Device* left, ID3D11Device* right) {
  if (!left || !right) {
    return false;
  }
  Microsoft::WRL::ComPtr<IUnknown> left_identity;
  Microsoft::WRL::ComPtr<IUnknown> right_identity;
  return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity))) &&
         SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity))) &&
         left_identity.Get() == right_identity.Get();
}

}  // namespace

bool WindowsD3D11TargetRing::install(const void* const* textures,
                                    size_t texture_count,
                                    void* displayed_texture,
                                    void* protected_texture,
                                    int width,
                                    int height,
                                    DXGI_FORMAT format,
                                    std::string& error) {
  error.clear();
  if (!textures || texture_count < kMinTargetCount ||
      texture_count > kMaxTargetCount) {
    error = "D3D11 native target ring requires 3 to 8 textures";
    return false;
  }
  if (width <= 0 || height <= 0 || !supported_target_format(format)) {
    error = "D3D11 native target ring has invalid dimensions or format";
    return false;
  }

  std::vector<Slot> next_slots;
  next_slots.reserve(texture_count);
  std::unordered_set<ID3D11Texture2D*> unique_textures;
  Microsoft::WRL::ComPtr<ID3D11Device> ring_device;
  for (size_t index = 0; index < texture_count; ++index) {
    auto* texture = static_cast<ID3D11Texture2D*>(
        const_cast<void*>(textures[index]));
    if (!texture || !unique_textures.insert(texture).second) {
      error = "D3D11 native target ring contains a null or duplicate texture";
      return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    const UINT required_bind_flags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (desc.Width != static_cast<UINT>(width) ||
        desc.Height != static_cast<UINT>(height) || desc.Format != format ||
        desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 ||
        (desc.BindFlags & required_bind_flags) != required_bind_flags) {
      error = "D3D11 native target texture does not match the viewport contract";
      return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> texture_device;
    texture->GetDevice(&texture_device);
    if (!texture_device) {
      error = "D3D11 native target texture has no owning device";
      return false;
    }
    if (!ring_device) {
      ring_device = texture_device;
    } else if (!same_com_identity(ring_device.Get(), texture_device.Get())) {
      error = "D3D11 native target textures must share one device";
      return false;
    }

    Slot slot;
    slot.texture = texture;
    if (texture == displayed_texture) {
      slot.state = WindowsD3D11TargetState::Displayed;
    } else if (texture == protected_texture) {
      slot.state = WindowsD3D11TargetState::Protected;
    }
    next_slots.push_back(std::move(slot));
  }

  if ((displayed_texture &&
       unique_textures.count(static_cast<ID3D11Texture2D*>(displayed_texture)) == 0) ||
      (protected_texture &&
       unique_textures.count(static_cast<ID3D11Texture2D*>(protected_texture)) == 0)) {
    error = "D3D11 displayed or protected target is outside the ring";
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  slots_ = std::move(next_slots);
  device_ = std::move(ring_device);
  displayed_ = static_cast<ID3D11Texture2D*>(displayed_texture);
  protected_ = static_cast<ID3D11Texture2D*>(protected_texture);
  width_ = width;
  height_ = height;
  format_ = format;
  ++generation_;
  return true;
}

void WindowsD3D11TargetRing::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  slots_.clear();
  device_.Reset();
  displayed_ = nullptr;
  protected_ = nullptr;
  width_ = 0;
  height_ = 0;
  format_ = DXGI_FORMAT_UNKNOWN;
  ++generation_;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D>
WindowsD3D11TargetRing::acquire_draw_target() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& slot : slots_) {
    if (slot.state == WindowsD3D11TargetState::Available && slot.texture) {
      slot.state = WindowsD3D11TargetState::InFlight;
      ++acquisition_count_;
      return slot.texture;
    }
  }
  ++backpressure_count_;
  return {};
}

bool WindowsD3D11TargetRing::complete_draw_target(ID3D11Texture2D* texture,
                                                  bool success) {
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = find_locked(texture);
  if (!slot || slot->state != WindowsD3D11TargetState::InFlight) {
    return false;
  }
  if (success) {
    slot->state = WindowsD3D11TargetState::Completed;
    ++completion_count_;
  } else if (texture == displayed_) {
    slot->state = WindowsD3D11TargetState::Displayed;
  } else if (texture == protected_) {
    slot->state = WindowsD3D11TargetState::Protected;
  } else {
    slot->state = WindowsD3D11TargetState::Available;
  }
  return true;
}

void WindowsD3D11TargetRing::mark_displayed(ID3D11Texture2D* texture) {
  if (!texture) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!find_locked(texture)) {
    return;
  }
  displayed_ = texture;
  restore_retained_states_locked();
}

void WindowsD3D11TargetRing::protect(ID3D11Texture2D* texture) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (texture && !find_locked(texture)) {
    return;
  }
  protected_ = texture;
  restore_retained_states_locked();
}

void WindowsD3D11TargetRing::release(ID3D11Texture2D* texture) {
  if (!texture) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = find_locked(texture);
  if (!slot || slot->state != WindowsD3D11TargetState::Completed) {
    ++release_miss_count_;
    return;
  }
  ++release_count_;
  if (texture == displayed_) {
    slot->state = WindowsD3D11TargetState::Displayed;
  } else if (texture == protected_) {
    slot->state = WindowsD3D11TargetState::Protected;
  } else {
    slot->state = WindowsD3D11TargetState::Available;
  }
}

bool WindowsD3D11TargetRing::contains(ID3D11Texture2D* texture) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return find_locked(texture) != nullptr;
}

bool WindowsD3D11TargetRing::installed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return slots_.size() >= kMinTargetCount && device_;
}

Microsoft::WRL::ComPtr<ID3D11Device> WindowsD3D11TargetRing::device() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return device_;
}

WindowsD3D11TargetState WindowsD3D11TargetRing::state_for_test(
    ID3D11Texture2D* texture) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Slot* slot = find_locked(texture);
  return slot ? slot->state : WindowsD3D11TargetState::Available;
}

WindowsD3D11TargetRingDiagnostics WindowsD3D11TargetRing::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  WindowsD3D11TargetRingDiagnostics result;
  result.target_count = slots_.size();
  result.width = width_;
  result.height = height_;
  result.format = format_;
  result.generation = generation_;
  result.acquisition_count = acquisition_count_;
  result.completion_count = completion_count_;
  result.release_count = release_count_;
  result.release_miss_count = release_miss_count_;
  result.backpressure_count = backpressure_count_;
  for (const auto& slot : slots_) {
    switch (slot.state) {
      case WindowsD3D11TargetState::Available:
        ++result.available_count;
        break;
      case WindowsD3D11TargetState::InFlight:
        ++result.in_flight_count;
        break;
      case WindowsD3D11TargetState::Completed:
        ++result.completed_count;
        break;
      case WindowsD3D11TargetState::Displayed:
      case WindowsD3D11TargetState::Protected:
        break;
    }
  }
  return result;
}

WindowsD3D11TargetRing::Slot* WindowsD3D11TargetRing::find_locked(
    ID3D11Texture2D* texture) {
  const auto found = std::find_if(slots_.begin(), slots_.end(), [texture](const Slot& slot) {
    return slot.texture.Get() == texture;
  });
  return found == slots_.end() ? nullptr : &*found;
}

const WindowsD3D11TargetRing::Slot* WindowsD3D11TargetRing::find_locked(
    ID3D11Texture2D* texture) const {
  const auto found = std::find_if(slots_.begin(), slots_.end(), [texture](const Slot& slot) {
    return slot.texture.Get() == texture;
  });
  return found == slots_.end() ? nullptr : &*found;
}

void WindowsD3D11TargetRing::restore_retained_states_locked() {
  for (auto& slot : slots_) {
    if (slot.state == WindowsD3D11TargetState::InFlight) {
      continue;
    }
    if (slot.texture.Get() == displayed_) {
      slot.state = WindowsD3D11TargetState::Displayed;
    } else if (slot.texture.Get() == protected_) {
      slot.state = WindowsD3D11TargetState::Protected;
    } else if (slot.state == WindowsD3D11TargetState::Displayed ||
               slot.state == WindowsD3D11TargetState::Protected) {
      slot.state = WindowsD3D11TargetState::Available;
    }
  }
}

}  // namespace vr
