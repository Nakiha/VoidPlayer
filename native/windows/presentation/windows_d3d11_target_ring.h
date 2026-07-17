#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vr {

enum class WindowsD3D11TargetState : uint8_t {
  Available,
  InFlight,
  Completed,
  Displayed,
  Protected,
};

struct WindowsD3D11TargetRingDiagnostics {
  size_t target_count = 0;
  size_t available_count = 0;
  size_t in_flight_count = 0;
  size_t completed_count = 0;
  int width = 0;
  int height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint64_t generation = 0;
  uint64_t acquisition_count = 0;
  uint64_t completion_count = 0;
  uint64_t release_count = 0;
  uint64_t release_miss_count = 0;
  uint64_t backpressure_count = 0;
};

struct WindowsD3D11TargetRingInstall {
  const void* const* textures = nullptr;
  size_t texture_count = 0;
  void* displayed_texture = nullptr;
  void* protected_texture = nullptr;
  int width = 0;
  int height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// Tracks runner-owned complete-viewport textures. The presentation backend may
// render only into Available targets; the runner marks the texture currently
// sampled by the final compositor as Displayed and may protect one prior target
// while a native composition command is still using it.
class WindowsD3D11TargetRing {
 public:
  static constexpr size_t kMinTargetCount = 3;
  static constexpr size_t kMaxTargetCount = 8;

  bool install(const void* const* textures,
               size_t texture_count,
               void* displayed_texture,
               void* protected_texture,
               int width,
               int height,
               DXGI_FORMAT format,
               std::string& error);
  void clear();

  Microsoft::WRL::ComPtr<ID3D11Texture2D> acquire_draw_target();
  bool complete_draw_target(ID3D11Texture2D* texture, bool success);
  void mark_displayed(ID3D11Texture2D* texture);
  void protect(ID3D11Texture2D* texture);
  void release(ID3D11Texture2D* texture);

  bool contains(ID3D11Texture2D* texture) const;
  bool installed() const;
  Microsoft::WRL::ComPtr<ID3D11Device> device() const;
  WindowsD3D11TargetState state_for_test(ID3D11Texture2D* texture) const;
  WindowsD3D11TargetRingDiagnostics diagnostics() const;

 private:
  struct Slot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    WindowsD3D11TargetState state = WindowsD3D11TargetState::Available;
  };

  Slot* find_locked(ID3D11Texture2D* texture);
  const Slot* find_locked(ID3D11Texture2D* texture) const;
  void restore_retained_states_locked();

  mutable std::mutex mutex_;
  std::vector<Slot> slots_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  ID3D11Texture2D* displayed_ = nullptr;
  ID3D11Texture2D* protected_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  uint64_t generation_ = 0;
  uint64_t acquisition_count_ = 0;
  uint64_t completion_count_ = 0;
  uint64_t release_count_ = 0;
  uint64_t release_miss_count_ = 0;
  uint64_t backpressure_count_ = 0;
};

}  // namespace vr
