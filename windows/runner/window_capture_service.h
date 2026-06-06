#pragma once

#include <windows.h>

#include <string>

struct WindowCaptureResult {
    std::string hash;
    int width = 0;
    int height = 0;
    double avg_luma = 0.0;
    double non_black_ratio = 0.0;
    std::string output_path;
};

enum class WindowCaptureStatus {
    Ok,
    InvalidWindow,
    CaptureFailed,
    SaveFailed,
};

class WindowCaptureService {
public:
    WindowCaptureStatus Capture(HWND window,
                                const std::string& output_path,
                                WindowCaptureResult& result) const;
};
