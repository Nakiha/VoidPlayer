#pragma once

#include "audio/audio_output.h"
#include <memory>

namespace vr {

std::unique_ptr<AudioOutput> create_default_audio_output();

} // namespace vr
