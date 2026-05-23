#pragma once

namespace vr {

enum class PresentationBackendKind {
    D3D11 = 1,
};

struct PresentationBackendConfig {
    void* hwnd = nullptr;
    void* adapter = nullptr;
    int width = 0;
    int height = 0;
    bool headless = false;
};

class PresentationBackend {
public:
    virtual ~PresentationBackend() = default;

    virtual PresentationBackendKind kind() const = 0;
    virtual const char* name() const = 0;
    virtual bool initialize(const PresentationBackendConfig& config) = 0;
    virtual void shutdown() = 0;
    virtual bool headless() const = 0;
};

} // namespace vr
