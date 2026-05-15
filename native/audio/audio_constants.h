#pragma once

#include <cstddef>

namespace vr {

constexpr int kAudioOutputSampleRate = 48000;
constexpr int kAudioOutputChannels = 2;
constexpr int kAudioBytesPerSample = 2;
constexpr int kAudioOutputBufferFrames = 480;  // 10ms
constexpr int kAudioOutputBufferCount = 4;
constexpr size_t kAudioPcmCapacityFrames = kAudioOutputSampleRate / 2;  // 500ms
constexpr size_t kAudioFadeFrames = 480;  // 10ms

} // namespace vr
