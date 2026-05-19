#pragma once

#include <map>
#include <memory>

namespace vr {

class PcmBuffer;

class MiniaudioOutput {
public:
    MiniaudioOutput();
    ~MiniaudioOutput();

    MiniaudioOutput(const MiniaudioOutput&) = delete;
    MiniaudioOutput& operator=(const MiniaudioOutput&) = delete;

    void start();
    void stop();
    void set_playing(bool playing);
    void set_active_track(int file_id);
    int active_track() const;
    void set_tracks(const std::map<int, std::shared_ptr<PcmBuffer>>& tracks);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vr
