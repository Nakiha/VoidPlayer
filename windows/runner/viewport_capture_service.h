#pragma once

#include "player/native_player.h"

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
};
