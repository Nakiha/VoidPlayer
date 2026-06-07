#pragma once

#include "windows/player/native_player.h"

#include <string>

struct ViewportCaptureResult {
    std::string hash;
    int width = 0;
    int height = 0;
    double avg_luma = 0.0;
    double non_black_ratio = 0.0;
    std::string output_path;
};

enum class ViewportCaptureStatus {
    Ok,
    CaptureFailed,
    SaveFailed,
};

class ViewportCaptureService {
public:
    ViewportCaptureStatus Capture(vr::NativePlayer& player,
                                  const std::string& output_path,
                                  ViewportCaptureResult& result) const;

    ViewportCaptureStatus CaptureRegion(vr::NativePlayer& player,
                                        int x,
                                        int y,
                                        int width,
                                        int height,
                                        int max_size,
                                        const std::string& output_path,
                                        ViewportCaptureResult& result) const;
};
