#pragma once
#include "renderer/renderer_impl.h"
#include "renderer/overlay/analysis_overlay_renderer.h"
#include "renderer/layout/layout_geometry.h"
#include "renderer/layout/layout_validation.h"
#include "renderer/renderer_config_validation.h"
#include "renderer/playback/renderer_playback_command_policy.h"
#include "renderer/seek/renderer_seek_log_policy.h"
#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_preroll_policy.h"
#include "renderer/track/track_present_policy.h"
#include "renderer/track/track_preview_policy.h"
#include "renderer/track/track_step_policy.h"
#include "audio/audio_output_factory.h"
#include "renderer/audio_coordinator.h"
#include "renderer/seek/seek_coordinator.h"
#include "renderer/render/device_loss_policy.h"
#include "renderer/render/presentation_backend_factory.h"
#include "renderer/render/presentation_snapshot.h"
#include "renderer/render/render_thread_platform.h"
#include "renderer/render/renderer_timing_utils.h"
#include "renderer/render/renderer_viewport_trace.h"
#include "renderer/render/swap_chain_present_policy.h"
#include "renderer/track/track_snapshot.h"
#include <spdlog/spdlog.h>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <utility>

namespace vr {

static constexpr auto kPausedHevcSeekSettleDelay = std::chrono::milliseconds(250);
static constexpr auto kStepForwardDecodeWait = std::chrono::milliseconds(180);
static constexpr auto kStepBackwardReconstructionWait = std::chrono::milliseconds(250);
} // namespace vr
