#include "device.h"
#include <spdlog/spdlog.h>
#include <dxgidebug.h>

namespace vr {

D3D11Device::D3D11Device() = default;

D3D11Device::~D3D11Device() {
    shutdown();
}

static void report_live_objects(ID3D11Device* device) {
    if (!device) return;
    ID3D11Debug* debug = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(ID3D11Debug), reinterpret_cast<void**>(&debug));
    if (SUCCEEDED(hr) && debug) {
        hr = debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
        debug->Release();
    }
}

bool D3D11Device::create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type,
                                UINT create_device_flags, D3D_FEATURE_LEVEL& out_level) {
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };

    HRESULT hr = D3D11CreateDevice(
        adapter,
        driver_type,
        nullptr,
        create_device_flags,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        device_.GetAddressOf(),
        &out_level,
        context_.GetAddressOf()
    );

    if (FAILED(hr)) {
        spdlog::error("Failed to create D3D11 device: HRESULT {:#x}", static_cast<unsigned long>(hr));
        return false;
    }

    spdlog::info("[D3D11] Device created: driver_type={}, feature_level={}.{}",
                 static_cast<int>(driver_type),
                 static_cast<int>(out_level) >> 12,
                 (static_cast<int>(out_level) >> 8) & 0xF);
    return true;
}

void D3D11Device::setup_info_queue() {
    ID3D11InfoQueue* info_queue = nullptr;
    HRESULT hr = device_->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&info_queue));
    if (SUCCEEDED(hr) && info_queue) {
        info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
        info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
        D3D11_MESSAGE_ID hide[] = {
            D3D11_MESSAGE_ID_SETPRIVATEDATA_CHANGINGPARAMS,
        };
        D3D11_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = hide;
        info_queue->AddStorageFilterEntries(&filter);
        info_queue->Release();
        spdlog::info("[D3D11] Debug info queue enabled");
    }
}

bool D3D11Device::initialize(void* hwnd, int width, int height) {
    if (initialized_) {
        spdlog::warn("D3D11Device already initialized");
        return true;
    }
    hwnd_ = hwnd;
    headless_ = false;

    UINT create_device_flags = 0;
    create_device_flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    D3D_DRIVER_TYPE driver_types[] = {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };

    D3D_FEATURE_LEVEL obtained_level = D3D_FEATURE_LEVEL_9_1;
    bool created = false;
    for (UINT i = 0; i < ARRAYSIZE(driver_types); ++i) {
        device_.Reset();
        context_.Reset();
        if (create_device(nullptr, driver_types[i], create_device_flags, obtained_level)) {
            created = true;
            break;
        }
    }
    if (!created) return false;

    // Create swap chain
    IDXGIDevice* dxgi_device = nullptr;
    HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
    if (FAILED(hr)) {
        spdlog::error("Failed to query DXGI device: HRESULT {:#x}", static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgi_device->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter));
    dxgi_device->Release();
    if (FAILED(hr)) {
        spdlog::error("Failed to get DXGI adapter: HRESULT {:#x}", static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    IDXGIFactory* factory = nullptr;
    hr = adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    adapter->Release();
    if (FAILED(hr)) {
        spdlog::error("Failed to get DXGI factory: HRESULT {:#x}", static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sc_desc = {};
    sc_desc.BufferCount = 2;
    sc_desc.BufferDesc.Width = static_cast<UINT>(width);
    sc_desc.BufferDesc.Height = static_cast<UINT>(height);
    sc_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.BufferDesc.RefreshRate.Numerator = 60;
    sc_desc.BufferDesc.RefreshRate.Denominator = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.OutputWindow = static_cast<HWND>(hwnd);
    sc_desc.Windowed = TRUE;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.SampleDesc.Quality = 0;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = factory->CreateSwapChain(device_.Get(), &sc_desc, swap_chain_.GetAddressOf());
    factory->Release();

    if (FAILED(hr)) {
        spdlog::error("Failed to create swap chain: HRESULT {:#x}", static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    initialized_ = true;
    spdlog::info("[D3D11] Device initialized successfully ({}x{})", width, height);
    setup_info_queue();
    return true;
}

bool D3D11Device::initialize_headless(IDXGIAdapter* adapter, int width, int height) {
    if (initialized_) {
        spdlog::warn("D3D11Device already initialized");
        return true;
    }
    headless_ = true;

    UINT create_device_flags = 0;
    create_device_flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    D3D_FEATURE_LEVEL obtained_level = D3D_FEATURE_LEVEL_9_1;
    // When passing an adapter, driver type must be D3D_DRIVER_TYPE_UNKNOWN
    if (!create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, create_device_flags, obtained_level)) {
        return false;
    }

    initialized_ = true;
    spdlog::info("[D3D11] Headless device initialized successfully ({}x{})", width, height);
    setup_info_queue();
    return true;
}

void D3D11Device::shutdown() {
    if (!initialized_) {
        return;
    }

    dump_debug_messages();

    swap_chain_.Reset();
    context_.Reset();
    device_.Reset();

    initialized_ = false;
    spdlog::info("D3D11 device shut down");
}

void D3D11Device::resize(int width, int height) {
    if (headless_) return;

    if (!swap_chain_) {
        spdlog::warn("Cannot resize: swap chain is null");
        return;
    }

    HRESULT hr = swap_chain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                             DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        spdlog::error("Failed to resize swap chain buffers: HRESULT {:#x}", static_cast<unsigned long>(hr));
    } else {
        spdlog::info("Swap chain resized to {}x{}", width, height);
    }
}

void D3D11Device::present(int sync_interval) {
    if (headless_) return;

    if (!swap_chain_) {
        spdlog::warn("Cannot present: swap chain is null");
        return;
    }

    if (hwnd_ && !IsWindowVisible(static_cast<HWND>(hwnd_))) {
        return;
    }

    HRESULT hr = swap_chain_->Present(sync_interval, 0);
    if (FAILED(hr)) {
        spdlog::error("Failed to present: HRESULT {:#x}", static_cast<unsigned long>(hr));
    }
}

void D3D11Device::dump_debug_messages() {
    if (!device_) return;
    ID3D11InfoQueue* iq = nullptr;
    HRESULT hr = device_->QueryInterface(__uuidof(ID3D11InfoQueue),
                                          reinterpret_cast<void**>(&iq));
    if (FAILED(hr) || !iq) return;

    UINT64 count = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T msg_len = 0;
        iq->GetMessage(i, nullptr, &msg_len);
        if (msg_len == 0) continue;
        auto* msg = static_cast<D3D11_MESSAGE*>(std::malloc(msg_len));
        if (!msg) continue;
        if (SUCCEEDED(iq->GetMessage(i, msg, &msg_len))) {
            const char* severity = "INFO";
            switch (msg->Severity) {
                case D3D11_MESSAGE_SEVERITY_CORRUPTION: severity = "CORRUPTION"; break;
                case D3D11_MESSAGE_SEVERITY_ERROR:      severity = "ERROR"; break;
                case D3D11_MESSAGE_SEVERITY_WARNING:    severity = "WARNING"; break;
                case D3D11_MESSAGE_SEVERITY_INFO:       severity = "INFO"; break;
                default: break;
            }
            spdlog::warn("[D3D11][{}] {} (id={})",
                         severity,
                         std::string(msg->pDescription, msg->DescriptionByteLength),
                         static_cast<int>(msg->ID));
        }
        std::free(msg);
    }
    iq->ClearStoredMessages();
    iq->Release();
}

} // namespace vr
