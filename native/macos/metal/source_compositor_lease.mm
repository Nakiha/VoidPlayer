#include "macos/player/native_player_bridge.h"

#include "renderer/render/source_compositor_contract.h"

#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

void write_error(char* error, size_t error_size, const std::string& message) {
  if (!error || error_size == 0) {
    return;
  }
  std::snprintf(error, error_size, "%s", message.c_str());
}

class PixelBufferRef {
 public:
  PixelBufferRef() = default;
  explicit PixelBufferRef(CVPixelBufferRef value) : value_(value) {}
  ~PixelBufferRef() {
    if (value_) {
      CFRelease(value_);
    }
  }

  PixelBufferRef(const PixelBufferRef&) = delete;
  PixelBufferRef& operator=(const PixelBufferRef&) = delete;

  PixelBufferRef(PixelBufferRef&& other) noexcept : value_(other.value_) {
    other.value_ = nullptr;
  }

  PixelBufferRef& operator=(PixelBufferRef&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (value_) {
      CFRelease(value_);
    }
    value_ = other.value_;
    other.value_ = nullptr;
    return *this;
  }

  CVPixelBufferRef get() const { return value_; }

 private:
  CVPixelBufferRef value_ = nullptr;
};

struct BackendDeleter {
  void operator()(VPMacOSMetalPresentationBackend* backend) const {
    VPMacOSMetalPresentationBackendDestroy(backend);
  }
};

struct TrackRing {
  vr::SourceCompositorTrackDescriptor descriptor;
  std::vector<PixelBufferRef> buffers;
  int published_index = 0;
  VPMacOSNativeFrameInfo published_frame_info{};
};

struct SourceLeaseState {
  vr::SourceCompositorOutputContract output;
  vr::SourceCompositorRingPolicy policy;
  vr::SourceCompositorLifecycle lifecycle;
  std::vector<TrackRing> tracks;
  std::unique_ptr<VPMacOSMetalPresentationBackend, BackendDeleter> backend;
  uint64_t required_slot_mask = 0;
  uint64_t published_slot_mask = 0;
};

uint64_t slot_mask(
    const std::vector<vr::SourceCompositorTrackDescriptor>& descriptors) {
  uint64_t result = 0;
  for (const auto& descriptor : descriptors) {
    result |= uint64_t{1} << static_cast<uint64_t>(descriptor.slot);
  }
  return result;
}

bool apply_lifecycle(SourceLeaseState& state,
                     vr::SourceCompositorLifecycleEventType type,
                     uint64_t topology_generation,
                     uint64_t ring_generation,
                     uint64_t frame_generation,
                     std::string& error) {
  vr::SourceCompositorLifecycleEvent event;
  event.type = type;
  event.topology_generation = topology_generation;
  event.ring_generation = ring_generation;
  event.frame_generation = frame_generation;
  if (vr::apply_source_compositor_lifecycle_event(state.lifecycle, event)) {
    return true;
  }
  error = "source compositor lifecycle transition rejected";
  return false;
}

void drain_state(SourceLeaseState& state) {
  std::string ignored;
  if (state.lifecycle.state !=
          vr::SourceCompositorLifecycleState::Unconfigured &&
      state.lifecycle.state != vr::SourceCompositorLifecycleState::Draining) {
    apply_lifecycle(state,
                    vr::SourceCompositorLifecycleEventType::BeginDraining,
                    state.lifecycle.topology_generation,
                    state.lifecycle.ring_generation,
                    state.lifecycle.frame_generation,
                    ignored);
  }
  apply_lifecycle(state,
                  vr::SourceCompositorLifecycleEventType::Reset,
                  0,
                  0,
                  0,
                  ignored);
}

std::unique_ptr<SourceLeaseState> build_state(
    const VPMacOSNativeSourceCompositorDescriptor* descriptors,
    size_t descriptor_count,
    bool edr_output_enabled,
    uint64_t topology_generation,
    uint64_t ring_generation,
    std::string& error) {
  if (!descriptors || descriptor_count == 0 ||
      descriptor_count > static_cast<size_t>(vr::kSourceCompositorMaxTracks) ||
      topology_generation == 0 || ring_generation == 0) {
    error = "invalid source compositor subscription";
    return nullptr;
  }

  std::vector<vr::SourceCompositorTrackDescriptor> shared_descriptors;
  shared_descriptors.reserve(descriptor_count);
  int max_width = 1;
  int max_height = 1;
  for (size_t i = 0; i < descriptor_count; ++i) {
    vr::SourceCompositorTrackDescriptor descriptor;
    descriptor.slot = descriptors[i].source_slot;
    descriptor.file_id = descriptors[i].source_file_id;
    descriptor.width = descriptors[i].width;
    descriptor.height = descriptors[i].height;
    descriptor.color_transfer = descriptors[i].color_transfer;
    shared_descriptors.push_back(descriptor);
    max_width = std::max(max_width, descriptor.width);
    max_height = std::max(max_height, descriptor.height);
  }
  if (!vr::validate_source_compositor_descriptors(shared_descriptors)) {
    error = "invalid source compositor descriptors";
    return nullptr;
  }

  auto state = std::make_unique<SourceLeaseState>();
  state->output = edr_output_enabled
      ? vr::source_compositor_edr_output_contract()
      : vr::source_compositor_sdr_output_contract();
  state->policy = vr::resolve_source_compositor_ring_policy(
      shared_descriptors,
      vr::kSourceCompositorDefaultBudgetBytes,
      state->output.bytes_per_pixel);
  if (!state->policy.allowed || state->policy.depth <= 0) {
    error = "source compositor memory budget exceeded";
    return nullptr;
  }
  if (!apply_lifecycle(*state,
                       vr::SourceCompositorLifecycleEventType::BeginAllocation,
                       topology_generation,
                       0,
                       0,
                       error)) {
    return nullptr;
  }

  const OSType pixel_format = edr_output_enabled
      ? kCVPixelFormatType_64RGBAHalf
      : kCVPixelFormatType_32BGRA;
  NSDictionary* attributes = @{
    (NSString*)kCVPixelBufferMetalCompatibilityKey : @YES,
    (NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  state->tracks.reserve(shared_descriptors.size());
  for (const auto& descriptor : shared_descriptors) {
    TrackRing ring;
    ring.descriptor = descriptor;
    VPMacOSNativeFrameInfoInit(&ring.published_frame_info);
    ring.buffers.reserve(static_cast<size_t>(state->policy.depth));
    for (int index = 0; index < state->policy.depth; ++index) {
      CVPixelBufferRef pixel_buffer = nullptr;
      const CVReturn status = CVPixelBufferCreate(
          kCFAllocatorDefault,
          descriptor.width,
          descriptor.height,
          pixel_format,
          (__bridge CFDictionaryRef)attributes,
          &pixel_buffer);
      if (status != kCVReturnSuccess || !pixel_buffer) {
        error = "failed to allocate native source compositor IOSurface";
        return nullptr;
      }
      ring.buffers.emplace_back(pixel_buffer);
    }
    state->tracks.push_back(std::move(ring));
  }
  state->backend.reset(
      VPMacOSMetalPresentationBackendCreate(max_width, max_height));
  if (!state->backend ||
      VPMacOSMetalPresentationBackendIsAvailable(state->backend.get()) == 0) {
    error = "native source compositor Metal backend is unavailable";
    return nullptr;
  }
  state->required_slot_mask = slot_mask(shared_descriptors);
  if (!apply_lifecycle(*state,
                       vr::SourceCompositorLifecycleEventType::MarkReady,
                       topology_generation,
                       ring_generation,
                       0,
                       error)) {
    return nullptr;
  }
  return state;
}

void copy_package(SourceLeaseState& state,
                  VPMacOSNativeSourceCompositorPackage* out) {
  VPMacOSNativeSourceCompositorPackageInit(out);
  out->topology_generation = state.lifecycle.topology_generation;
  out->ring_generation = state.lifecycle.ring_generation;
  out->frame_generation = state.lifecycle.frame_generation;
  out->publish_count = state.lifecycle.publish_count;
  out->ring_depth = state.policy.depth;
  out->track_count = static_cast<int32_t>(state.tracks.size());
  out->required_slot_mask = state.required_slot_mask;
  out->published_slot_mask = state.published_slot_mask;
  out->frozen_snapshot = state.policy.frozen_snapshot ? 1 : 0;
  for (size_t i = 0; i < state.tracks.size(); ++i) {
    const TrackRing& ring = state.tracks[i];
    CVPixelBufferRef pixel_buffer = ring.buffers[ring.published_index].get();
    CFRetain(pixel_buffer);
    out->entries[i].pixel_buffer = pixel_buffer;
    out->entries[i].source_slot = ring.descriptor.slot;
    out->entries[i].source_file_id = ring.descriptor.file_id;
    out->entries[i].width = ring.descriptor.width;
    out->entries[i].height = ring.descriptor.height;
    out->entries[i].frame_info = ring.published_frame_info;
  }
}

int bake_and_publish(SourceLeaseState& state,
                     VPMacOSNativePlayer* player,
                     bool advance,
                     VPMacOSNativeSourceCompositorPackage* out,
                     std::string& error) {
  if (!player || !out || state.tracks.empty() || !state.backend) {
    error = "source compositor lease is not ready";
    return VPMacOSNativeStatusInvalidArgument;
  }
  if (advance && state.policy.frozen_snapshot) {
    error = "source compositor lease is a frozen snapshot";
    return VPMacOSNativeStatusTransientBackpressure;
  }

  std::vector<int> write_indices;
  std::vector<VPMacOSNativeSourceFrameBakeTarget> targets;
  write_indices.reserve(state.tracks.size());
  targets.reserve(state.tracks.size());
  for (const auto& ring : state.tracks) {
    const int write_index = advance
        ? (ring.published_index + 1) % state.policy.depth
        : ring.published_index;
    write_indices.push_back(write_index);
    VPMacOSNativeSourceFrameBakeTarget target{};
    target.pixel_buffer = ring.buffers[write_index].get();
    target.source_slot = ring.descriptor.slot;
    target.source_file_id = ring.descriptor.file_id;
    target.width = ring.descriptor.width;
    target.height = ring.descriptor.height;
    VPMacOSNativeFrameInfoInit(&target.frame_info);
    targets.push_back(target);
  }

  std::array<char, 512> bake_error{};
  const int drawn_count = VPMacOSNativePlayerBakeCurrentFrameSources(
      player,
      state.backend.get(),
      targets.data(),
      targets.size(),
      bake_error.data(),
      bake_error.size());
  if (drawn_count <= 0) {
    error = bake_error[0] ? bake_error.data()
                          : "source compositor bake produced no frames";
    return VPMacOSNativeStatusTransientBackpressure;
  }

  uint64_t published_mask = 0;
  for (size_t i = 0; i < targets.size(); ++i) {
    if (!targets[i].drawn) {
      continue;
    }
    if (targets[i].source_file_id != state.tracks[i].descriptor.file_id) {
      error = "source compositor bake returned mismatched file identity";
      return VPMacOSNativeStatusRendererFailed;
    }
    published_mask |= uint64_t{1}
        << static_cast<uint64_t>(state.tracks[i].descriptor.slot);
  }
  if (published_mask != state.required_slot_mask) {
    error = "source compositor suppressed an incomplete package";
    return VPMacOSNativeStatusTransientBackpressure;
  }

  std::vector<vr::SourceCompositorTrackDescriptor> descriptors;
  descriptors.reserve(state.tracks.size());
  for (const auto& ring : state.tracks) {
    descriptors.push_back(ring.descriptor);
  }
  vr::SourceCompositorPackageMetadata metadata;
  metadata.output = state.output;
  metadata.topology_generation = state.lifecycle.topology_generation;
  metadata.ring_generation = state.lifecycle.ring_generation;
  metadata.frame_generation = state.lifecycle.frame_generation + 1;
  metadata.buffer_index = write_indices.front();
  metadata.ring_depth = state.policy.depth;
  metadata.track_count = state.tracks.size();
  metadata.required_slot_mask = state.required_slot_mask;
  metadata.published_slot_mask = published_mask;
  metadata.frozen_snapshot = state.policy.frozen_snapshot;
  if (!vr::validate_source_compositor_package(metadata, descriptors)) {
    error = "source compositor package failed shared contract validation";
    return VPMacOSNativeStatusRendererFailed;
  }
  if (!apply_lifecycle(state,
                       vr::SourceCompositorLifecycleEventType::Publish,
                       metadata.topology_generation,
                       metadata.ring_generation,
                       metadata.frame_generation,
                       error)) {
    return VPMacOSNativeStatusRendererFailed;
  }
  for (size_t i = 0; i < state.tracks.size(); ++i) {
    state.tracks[i].published_index = write_indices[i];
    state.tracks[i].published_frame_info = targets[i].frame_info;
  }
  state.published_slot_mask = published_mask;
  copy_package(state, out);
  return VPMacOSNativeStatusOk;
}

}  // namespace

struct VPMacOSNativeSourceCompositorLease {
  std::mutex mutex;
  std::unique_ptr<SourceLeaseState> state;
  uint64_t next_ring_generation = 0;
  uint64_t incomplete_publish_count = 0;
};

VPMacOSNativeSourceCompositorLease*
VPMacOSNativeSourceCompositorLeaseCreate(void) {
  return new VPMacOSNativeSourceCompositorLease();
}

void VPMacOSNativeSourceCompositorLeaseDestroy(
    VPMacOSNativeSourceCompositorLease* lease) {
  if (lease && lease->state) {
    drain_state(*lease->state);
  }
  delete lease;
}

int VPMacOSNativeSourceCompositorLeaseSubscribeAndBake(
    VPMacOSNativeSourceCompositorLease* lease,
    VPMacOSNativePlayer* player,
    const VPMacOSNativeSourceCompositorDescriptor* descriptors,
    size_t descriptor_count,
    int32_t edr_output_enabled,
    uint64_t topology_generation,
    VPMacOSNativeSourceCompositorPackage* out,
    char* error,
    size_t error_size) {
  if (!lease || !player || !out) {
    write_error(error, error_size, "invalid source compositor lease arguments");
    return VPMacOSNativeStatusInvalidArgument;
  }
  VPMacOSNativeSourceCompositorPackageInit(out);
  std::lock_guard<std::mutex> lock(lease->mutex);
  std::string message;
  const uint64_t ring_generation = lease->next_ring_generation + 1;
  auto candidate = build_state(
      descriptors,
      descriptor_count,
      edr_output_enabled != 0,
      topology_generation,
      ring_generation,
      message);
  if (!candidate) {
    write_error(error, error_size, message);
    return VPMacOSNativeStatusInvalidArgument;
  }
  const int status =
      bake_and_publish(*candidate, player, false, out, message);
  if (status != VPMacOSNativeStatusOk) {
    ++lease->incomplete_publish_count;
    write_error(error, error_size, message);
    return status;
  }
  lease->next_ring_generation = ring_generation;
  if (lease->state) {
    drain_state(*lease->state);
  }
  lease->state = std::move(candidate);
  write_error(error, error_size, "");
  return VPMacOSNativeStatusOk;
}

int VPMacOSNativeSourceCompositorLeaseRefreshAndBake(
    VPMacOSNativeSourceCompositorLease* lease,
    VPMacOSNativePlayer* player,
    VPMacOSNativeSourceCompositorPackage* out,
    char* error,
    size_t error_size) {
  if (!lease || !player || !out) {
    write_error(error, error_size, "invalid source compositor refresh arguments");
    return VPMacOSNativeStatusInvalidArgument;
  }
  VPMacOSNativeSourceCompositorPackageInit(out);
  std::lock_guard<std::mutex> lock(lease->mutex);
  if (!lease->state) {
    write_error(error, error_size, "source compositor lease is not subscribed");
    return VPMacOSNativeStatusInvalidArgument;
  }
  std::string message;
  const int status =
      bake_and_publish(*lease->state, player, true, out, message);
  if (status != VPMacOSNativeStatusOk) {
    ++lease->incomplete_publish_count;
  }
  write_error(error, error_size, message);
  return status;
}

int VPMacOSNativeSourceCompositorLeaseHasCompletePackage(
    VPMacOSNativeSourceCompositorLease* lease,
    uint64_t topology_generation) {
  if (!lease) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(lease->mutex);
  return lease->state &&
      lease->state->lifecycle.state ==
          vr::SourceCompositorLifecycleState::Publishing &&
      lease->state->lifecycle.topology_generation == topology_generation &&
      lease->state->published_slot_mask == lease->state->required_slot_mask;
}

void VPMacOSNativeSourceCompositorLeaseReset(
    VPMacOSNativeSourceCompositorLease* lease) {
  if (!lease) {
    return;
  }
  std::lock_guard<std::mutex> lock(lease->mutex);
  if (lease->state) {
    drain_state(*lease->state);
  }
  lease->state.reset();
}

int VPMacOSNativeSourceCompositorLeaseCopyDiagnostics(
    VPMacOSNativeSourceCompositorLease* lease,
    VPMacOSNativeSourceCompositorDiagnostics* out) {
  if (!lease || !out) {
    return VPMacOSNativeStatusInvalidArgument;
  }
  VPMacOSNativeSourceCompositorDiagnosticsInit(out);
  std::lock_guard<std::mutex> lock(lease->mutex);
  out->incomplete_publish_count = lease->incomplete_publish_count;
  if (!lease->state) {
    return VPMacOSNativeStatusOk;
  }
  const SourceLeaseState& state = *lease->state;
  out->lifecycle_state = static_cast<int32_t>(state.lifecycle.state);
  out->ring_depth = state.policy.depth;
  out->track_count = static_cast<int32_t>(state.tracks.size());
  out->frozen_snapshot = state.policy.frozen_snapshot ? 1 : 0;
  out->topology_generation = state.lifecycle.topology_generation;
  out->ring_generation = state.lifecycle.ring_generation;
  out->frame_generation = state.lifecycle.frame_generation;
  out->publish_count = state.lifecycle.publish_count;
  out->bytes_per_frame = state.policy.bytes_per_frame;
  out->total_bytes = state.policy.total_bytes;
  out->required_slot_mask = state.required_slot_mask;
  out->published_slot_mask = state.published_slot_mask;
  return VPMacOSNativeStatusOk;
}
