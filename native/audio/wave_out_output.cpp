#include "audio/wave_out_output.h"

#include "audio/audio_constants.h"
#include "audio/pcm_buffer.h"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vr {

WaveOutOutput::WaveOutOutput()
    : mixer_(kAudioOutputChannels, kAudioFadeFrames) {}

WaveOutOutput::~WaveOutOutput() {
    stop();
}

void WaveOutOutput::start() {
    if (running_.load()) return;
    running_.store(true);
    thread_ = std::thread(&WaveOutOutput::run, this);
}

void WaveOutOutput::stop() {
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void WaveOutOutput::set_playing(bool playing) {
    mixer_.set_playing(playing);
}

void WaveOutOutput::set_active_track(int file_id) {
    mixer_.set_active_track(file_id);
}

int WaveOutOutput::active_track() const {
    return mixer_.active_track();
}

void WaveOutOutput::set_tracks(
    const std::map<int, std::shared_ptr<PcmBuffer>>& tracks) {
    mixer_.set_tracks(tracks);
}

bool WaveOutOutput::open_device() {
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = kAudioOutputChannels;
    fmt.nSamplesPerSec = kAudioOutputSampleRate;
    fmt.wBitsPerSample = kAudioBytesPerSample * 8;
    fmt.nBlockAlign = kAudioOutputChannels * kAudioBytesPerSample;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    MMRESULT mm = waveOutOpen(&wave_out_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (mm != MMSYSERR_NOERROR) {
        spdlog::warn("[AudioOutput] waveOutOpen failed: {}", static_cast<unsigned>(mm));
        wave_out_ = nullptr;
        return false;
    }
    return true;
}

bool WaveOutOutput::check_wave_result(MMRESULT result, const char* operation) const {
    if (result == MMSYSERR_NOERROR) {
        return true;
    }
    spdlog::warn("[AudioOutput] {} failed: {}", operation, static_cast<unsigned>(result));
    return false;
}

bool WaveOutOutput::submit_header(WAVEHDR& header) {
    MMRESULT mm = waveOutPrepareHeader(wave_out_, &header, sizeof(WAVEHDR));
    if (!check_wave_result(mm, "waveOutPrepareHeader")) {
        return false;
    }
    mm = waveOutWrite(wave_out_, &header, sizeof(WAVEHDR));
    if (!check_wave_result(mm, "waveOutWrite")) {
        const MMRESULT unprepare = waveOutUnprepareHeader(wave_out_, &header, sizeof(WAVEHDR));
        check_wave_result(unprepare, "waveOutUnprepareHeader after failed write");
        return false;
    }
    return true;
}

void WaveOutOutput::unprepare_header(WAVEHDR& header) {
    if ((header.dwFlags & WHDR_PREPARED) == 0) {
        return;
    }
    const MMRESULT mm = waveOutUnprepareHeader(wave_out_, &header, sizeof(WAVEHDR));
    check_wave_result(mm, "waveOutUnprepareHeader");
}

void WaveOutOutput::run() {
    if (!open_device()) {
        running_.store(false);
        return;
    }
    const size_t bytes = kAudioOutputBufferFrames * kAudioOutputChannels * sizeof(int16_t);
    std::array<std::vector<int16_t>, kAudioOutputBufferCount> sample_buffers;
    std::array<WAVEHDR, kAudioOutputBufferCount> headers = {};
    for (int i = 0; i < kAudioOutputBufferCount; ++i) {
        sample_buffers[i].resize(kAudioOutputBufferFrames * kAudioOutputChannels);
        mixer_.render(sample_buffers[i].data(), kAudioOutputBufferFrames);
        headers[i].lpData = reinterpret_cast<LPSTR>(sample_buffers[i].data());
        headers[i].dwBufferLength = static_cast<DWORD>(bytes);
        if (!submit_header(headers[i])) {
            running_.store(false);
            break;
        }
    }

    while (running_.load()) {
        bool wrote = false;
        for (int i = 0; i < kAudioOutputBufferCount; ++i) {
            if ((headers[i].dwFlags & WHDR_DONE) == 0) continue;
            unprepare_header(headers[i]);
            std::memset(&headers[i], 0, sizeof(WAVEHDR));
            mixer_.render(sample_buffers[i].data(), kAudioOutputBufferFrames);
            headers[i].lpData = reinterpret_cast<LPSTR>(sample_buffers[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(bytes);
            if (!submit_header(headers[i])) {
                running_.store(false);
                break;
            }
            wrote = true;
        }
        if (!wrote) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    check_wave_result(waveOutReset(wave_out_), "waveOutReset");
    for (auto& header : headers) {
        unprepare_header(header);
    }
    check_wave_result(waveOutClose(wave_out_), "waveOutClose");
    wave_out_ = nullptr;
}

} // namespace vr
