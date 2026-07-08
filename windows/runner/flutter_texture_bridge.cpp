#include "flutter_texture_bridge.h"

#include <d3d11.h>
#include <new>
#include <utility>

namespace {

struct FlutterTextureReleaseContext {
    ID3D11Texture2D* texture = nullptr;
    std::weak_ptr<vr::NativePlayer> player;
    int buffer_index = -1;
    uint64_t buffer_generation = 0;
};

void ReleaseFlutterTexture(void* release_context) {
    auto* context = static_cast<FlutterTextureReleaseContext*>(release_context);
    if (!context) {
        return;
    }
    if (auto player = context->player.lock()) {
        player->release_shared_texture(
            context->buffer_index, context->buffer_generation);
    }
    if (context->texture) {
        context->texture->Release();
    }
    delete context;
}

} // namespace

FlutterTextureBridge::FlutterTextureBridge(
    flutter::TextureRegistrar* texture_registrar)
    : texture_registrar_(texture_registrar) {}

FlutterTextureBridge::~FlutterTextureBridge() {
    Unregister();
}

bool FlutterTextureBridge::Register(
    const std::shared_ptr<vr::NativePlayer>& player) {
    Unregister();
    if (!texture_registrar_ || !player) {
        return false;
    }

    player_ = player;
    surface_descriptor_ = {};
    surface_descriptor_.struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    surface_descriptor_.format = kFlutterDesktopPixelFormatBGRA8888;

    auto gpu_texture = std::make_unique<flutter::GpuSurfaceTexture>(
        kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
        [this](size_t width, size_t height) {
            return AcquireSurfaceDescriptor(width, height);
        });

    texture_variant_ = std::make_unique<flutter::TextureVariant>(std::move(*gpu_texture));
    const int64_t registered_id =
        texture_registrar_->RegisterTexture(texture_variant_.get());
    texture_id_.store(registered_id, std::memory_order_release);

    if (registered_id < 0) {
        texture_id_.store(-1, std::memory_order_release);
        texture_variant_.reset();
        player_.reset();
        return false;
    }

    player->set_frame_callback([this]() {
        MarkFrameAvailable();
    });
    frame_callback_attached_ = true;
    return true;
}

void FlutterTextureBridge::DetachFrameCallback() {
    if (!frame_callback_attached_) {
        return;
    }
    if (auto player = player_.lock()) {
        player->set_frame_callback(nullptr);
    }
    frame_callback_attached_ = false;
}

void FlutterTextureBridge::Unregister() {
    DetachFrameCallback();
    const int64_t registered_id =
        texture_id_.exchange(-1, std::memory_order_acq_rel);
    if (registered_id >= 0 && texture_registrar_) {
        texture_registrar_->UnregisterTexture(registered_id);
    }
    texture_variant_.reset();
    player_.reset();
}

int64_t FlutterTextureBridge::texture_id() const {
    return texture_id_.load(std::memory_order_acquire);
}

const FlutterDesktopGpuSurfaceDescriptor*
FlutterTextureBridge::AcquireSurfaceDescriptor(size_t, size_t) {
    auto player = player_.lock();
    if (!player) {
        return nullptr;
    }

    vr::SharedTextureSnapshot snapshot;
    if (!player->acquire_shared_texture(snapshot)) {
        return nullptr;
    }
    if (snapshot.type != vr::SharedTextureHandleType::D3D11SharedHandle ||
        !snapshot.texture ||
        !snapshot.handle) {
        return nullptr;
    }

    auto* texture = static_cast<ID3D11Texture2D*>(snapshot.texture);
    auto* release_context = new (std::nothrow) FlutterTextureReleaseContext{
        texture,
        player,
        snapshot.buffer_index,
        snapshot.buffer_generation};
    if (!release_context) {
        texture->Release();
        return nullptr;
    }

    surface_descriptor_.handle = snapshot.handle;
    surface_descriptor_.width = static_cast<size_t>(snapshot.width);
    surface_descriptor_.height = static_cast<size_t>(snapshot.height);
    surface_descriptor_.visible_width = surface_descriptor_.width;
    surface_descriptor_.visible_height = surface_descriptor_.height;
    surface_descriptor_.release_callback = ReleaseFlutterTexture;
    surface_descriptor_.release_context = release_context;
    return &surface_descriptor_;
}

void FlutterTextureBridge::MarkFrameAvailable() {
    const int64_t registered_id = texture_id_.load(std::memory_order_acquire);
    if (registered_id >= 0 && texture_registrar_) {
        texture_registrar_->MarkTextureFrameAvailable(registered_id);
    }
}
