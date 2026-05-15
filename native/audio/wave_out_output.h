#pragma once

#include "audio/audio_mixer.h"

#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <windows.h>
#include <mmsystem.h>

namespace vr {

class PcmBuffer;

class WaveOutOutput {
public:
    WaveOutOutput();
    ~WaveOutOutput();

    void start();
    void stop();
    void set_playing(bool playing);
    void set_active_track(int file_id);
    int active_track() const;
    void set_tracks(const std::map<int, std::shared_ptr<PcmBuffer>>& tracks);

private:
    bool open_device();
    bool check_wave_result(MMRESULT result, const char* operation) const;
    bool submit_header(WAVEHDR& header);
    void unprepare_header(WAVEHDR& header);
    void run();

    AudioMixer mixer_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    HWAVEOUT wave_out_ = nullptr;
};

} // namespace vr
