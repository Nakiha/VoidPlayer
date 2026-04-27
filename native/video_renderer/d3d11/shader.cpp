#include "shader.h"
#include <spdlog/spdlog.h>

namespace vr {

ShaderManager::ShaderManager(ID3D11Device* device)
    : device_(device) {
}

bool ShaderManager::compile_stage(const std::string& source, const std::string& entry,
                                   const std::string& target, ID3DBlob** out_blob) {
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob = nullptr;

    HRESULT hr = D3DCompile(
        source.c_str(),
        source.size(),
        nullptr,                        // source name
        nullptr,                        // defines
        nullptr,                        // include
        entry.c_str(),                  // entry point
        target.c_str(),                 // target (vs_5_0, ps_5_0)
        D3DCOMPILE_ENABLE_STRICTNESS,   // flags
        0,                              // flags2
        out_blob,                       // compiled code
        &error_blob                     // errors
    );

    if (FAILED(hr)) {
        if (error_blob) {
            std::string error_msg(static_cast<const char*>(error_blob->GetBufferPointer()),
                                  error_blob->GetBufferSize());
            spdlog::error("Shader compilation failed (target={}, entry={}): {}\nSource:\n{}",
                          target, entry, error_msg, source);
        } else {
            spdlog::error("Shader compilation failed (target={}, entry={}): HRESULT {:#x}",
                          target, entry, static_cast<unsigned long>(hr));
        }
        return false;
    }

    spdlog::debug("Compiled shader stage: {} / {} ({} bytes)",
                  target, entry, (*out_blob)->GetBufferSize());
    return true;
}

bool ShaderManager::compile_from_source(const std::string& source,
                                         const std::string& vs_entry,
                                         const std::string& ps_entry,
                                         CompiledShader& out) {
    if (!device_) {
        spdlog::error("Cannot compile shader: device is null");
        return false;
    }

    // Compile vertex shader
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob = nullptr;
    if (!compile_stage(source, vs_entry, "vs_5_0", &vs_blob)) {
        return false;
    }

    // Compile pixel shader
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob = nullptr;
    if (!compile_stage(source, ps_entry, "ps_5_0", &ps_blob)) {
        return false;
    }

    // Create vertex shader
    HRESULT hr = device_->CreateVertexShader(
        vs_blob->GetBufferPointer(),
        vs_blob->GetBufferSize(),
        nullptr,
        &out.vs
    );
    if (FAILED(hr)) {
        spdlog::error("Failed to create vertex shader: HRESULT {:#x}", static_cast<unsigned long>(hr));
        return false;
    }

    // Create pixel shader
    hr = device_->CreatePixelShader(
        ps_blob->GetBufferPointer(),
        ps_blob->GetBufferSize(),
        nullptr,
        &out.ps
    );
    if (FAILED(hr)) {
        spdlog::error("Failed to create pixel shader: HRESULT {:#x}", static_cast<unsigned long>(hr));
        return false;
    }

    // Create input layout from vertex shader blob
    if (!create_input_layout(device_.Get(), vs_blob.Get(), out)) {
        spdlog::warn("Failed to create input layout, shaders still available without layout");
    }

    spdlog::info("Compiled shader pair: VS '{}', PS '{}'", vs_entry, ps_entry);
    return true;
}

bool ShaderManager::create_input_layout(ID3D11Device* device, ID3DBlob* vs_blob, CompiledShader& out) {
    if (!device || !vs_blob) {
        spdlog::error("Cannot create input layout: device or VS blob is null");
        return false;
    }

    // Define the input layout for a fullscreen quad
    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    UINT num_elements = ARRAYSIZE(layout_desc);

    HRESULT hr = device->CreateInputLayout(
        layout_desc,
        num_elements,
        vs_blob->GetBufferPointer(),
        vs_blob->GetBufferSize(),
        &out.layout
    );

    if (FAILED(hr)) {
        spdlog::error("Failed to create input layout: HRESULT {:#x}", static_cast<unsigned long>(hr));
        return false;
    }

    spdlog::debug("Created input layout with {} elements", num_elements);
    return true;
}

bool ShaderManager::create_constant_buffer(ID3D11Device* device, UINT size, CompiledShader& out) {
    if (!device) {
        spdlog::error("Cannot create constant buffer: device is null");
        return false;
    }

    if (size == 0) {
        spdlog::error("Cannot create constant buffer: size is 0");
        return false;
    }

    // Constant buffer size must be a multiple of 16 bytes
    UINT aligned_size = ((size + 15) / 16) * 16;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = aligned_size;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    HRESULT hr = device->CreateBuffer(&desc, nullptr, &out.constant_buffer);
    if (FAILED(hr)) {
        spdlog::error("Failed to create constant buffer (size={}, aligned={}): HRESULT {:#x}",
                       size, aligned_size, static_cast<unsigned long>(hr));
        return false;
    }

    spdlog::debug("Created constant buffer: size={}, aligned={}", size, aligned_size);
    return true;
}

} // namespace vr
