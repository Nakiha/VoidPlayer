#include "audio/audio_output_factory.h"
#include "audio/audio_engine.h"

namespace vr {

std::unique_ptr<AudioOutput> create_default_audio_output() {
    return std::make_unique<AudioEngine>();
}

} // namespace vr
