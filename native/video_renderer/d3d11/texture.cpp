#include "texture.h"
#include <cstring>
#include <dxgi.h>
#include <spdlog/spdlog.h>

namespace vr {

TextureManager::TextureManager(ID3D11Device* device, ID3D11DeviceContext* context)
    : device_(device), context_(context) {
}

ID3D11Texture2D* TextureManager::create_rgba_texture(int width, int height) {
    if (!device_) {
        spdlog::error("Cannot create texture: device is null");
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = 0;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        spdlog::error("Failed to create RGBA texture ({}x{}): HRESULT {:#x}",
                       width, height, static_cast<unsigned long>(hr));
        return nullptr;
    }

    spdlog::debug("Created RGBA texture ({}x{})", width, height);
    return texture;
}

bool TextureManager::upload_data(ID3D11Texture2D* texture, const uint8_t* data,
                                  int width, int height, int stride) {
    if (!texture || !data || !context_) {
        spdlog::error("upload_data: invalid arguments (texture={}, data={}, context={})",
                       static_cast<void*>(texture), static_cast<const void*>(data),
                       static_cast<void*>(context_));
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context_->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        spdlog::error("Failed to map texture for upload: HRESULT {:#x}", static_cast<unsigned long>(hr));
        return false;
    }

    // Copy row by row to respect the subsurface pitch
    UINT src_pitch = static_cast<UINT>(stride);
    UINT dst_pitch = mapped.RowPitch;
    UINT row_bytes = static_cast<UINT>(width) * 4; // RGBA = 4 bytes per pixel

    const uint8_t* src_row = data;
    uint8_t* dst_row = static_cast<uint8_t*>(mapped.pData);

    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_row, src_row, row_bytes);
        src_row += src_pitch;
        dst_row += dst_pitch;
    }

    context_->Unmap(texture, 0);
    spdlog::trace("Uploaded texture data ({}x{}, stride={})", width, height, stride);
    return true;
}

ID3D11ShaderResourceView* TextureManager::create_srv(ID3D11Texture2D* texture) {
    if (!device_ || !texture) {
        spdlog::error("Cannot create SRV: device or texture is null");
        return nullptr;
    }

    // Get the texture format to use in the SRV description
    D3D11_TEXTURE2D_DESC tex_desc = {};
    texture->GetDesc(&tex_desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = tex_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = tex_desc.MipLevels;

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = device_->CreateShaderResourceView(texture, &srv_desc, &srv);
    if (FAILED(hr)) {
        spdlog::error("Failed to create shader resource view: HRESULT {:#x}",
                       static_cast<unsigned long>(hr));
        return nullptr;
    }

    spdlog::debug("Created shader resource view for texture");
    return srv;
}

bool TextureManager::open_shared_texture(
    ID3D11Texture2D* source,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& opened) {
    opened.Reset();
    if (!device_ || !source) {
        spdlog::error("open_shared_texture: invalid arguments (device={}, source={})",
                      static_cast<void*>(device_), static_cast<void*>(source));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIResource> dxgi_res;
    HRESULT hr = source->QueryInterface(__uuidof(IDXGIResource), &dxgi_res);
    if (FAILED(hr)) {
        spdlog::error("Failed to query IDXGIResource: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    HANDLE shared_handle = nullptr;
    hr = dxgi_res->GetSharedHandle(&shared_handle);
    if (FAILED(hr)) {
        spdlog::error("Failed to get shared texture handle: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    hr = device_->OpenSharedResource(
        shared_handle, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(opened.GetAddressOf()));
    if (FAILED(hr)) {
        spdlog::error("Failed to open shared texture: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    return opened != nullptr;
}

bool TextureManager::ensure_nv12_copy_resources(
    ID3D11Texture2D* source,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& copy_texture,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& y_srv,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& uv_srv,
    bool* created_new) {
    if (created_new) {
        *created_new = false;
    }
    if (!device_ || !source) {
        spdlog::error("ensure_nv12_copy_resources: invalid arguments (device={}, source={})",
                      static_cast<void*>(device_), static_cast<void*>(source));
        return false;
    }

    D3D11_TEXTURE2D_DESC src_desc = {};
    source->GetDesc(&src_desc);

    bool need_copy_tex = !copy_texture;
    if (copy_texture) {
        D3D11_TEXTURE2D_DESC copy_desc = {};
        copy_texture->GetDesc(&copy_desc);
        need_copy_tex =
            copy_desc.Width != src_desc.Width ||
            copy_desc.Height != src_desc.Height ||
            copy_desc.Format != src_desc.Format;
    }

    if (!need_copy_tex) {
        return y_srv && uv_srv;
    }

    copy_texture.Reset();
    y_srv.Reset();
    uv_srv.Reset();

    D3D11_TEXTURE2D_DESC copy_desc = src_desc;
    copy_desc.ArraySize = 1;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy_desc.CPUAccessFlags = 0;
    copy_desc.MiscFlags = 0;

    HRESULT hr = device_->CreateTexture2D(&copy_desc, nullptr, &copy_texture);
    if (FAILED(hr) || !copy_texture) {
        spdlog::error("Failed to create NV12 copy texture: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC y_desc = {};
    y_desc.Format = DXGI_FORMAT_R8_UNORM;
    y_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    y_desc.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(copy_texture.Get(), &y_desc, &y_srv);
    if (FAILED(hr)) {
        spdlog::error("Failed to create NV12 Y SRV: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        copy_texture.Reset();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC uv_desc = {};
    uv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    uv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uv_desc.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(copy_texture.Get(), &uv_desc, &uv_srv);
    if (FAILED(hr)) {
        spdlog::error("Failed to create NV12 UV SRV: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        copy_texture.Reset();
        y_srv.Reset();
        return false;
    }

    if (created_new) {
        *created_new = true;
    }
    spdlog::debug("Created NV12 copy texture ({}x{}, format={})",
                  src_desc.Width, src_desc.Height, static_cast<int>(src_desc.Format));
    return true;
}

} // namespace vr
