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

ID3D11Texture2D* TextureManager::create_plane_texture(int width, int height, bool is_16bit) {
    if (!device_) {
        spdlog::error("Cannot create plane texture: device is null");
        return nullptr;
    }
    if (width <= 0 || height <= 0) {
        spdlog::error("Cannot create plane texture: invalid geometry ({}x{})", width, height);
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = is_16bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = 0;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        spdlog::error("Failed to create {} plane texture ({}x{}): HRESULT {:#x}",
                      is_16bit ? "R16" : "R8", width, height,
                      static_cast<unsigned long>(hr));
        return nullptr;
    }

    spdlog::debug("Created {} plane texture ({}x{})", is_16bit ? "R16" : "R8", width, height);
    return texture;
}

ID3D11Texture2D* TextureManager::create_nv12_texture(int width, int height) {
    if (!device_) {
        spdlog::error("Cannot create NV12 texture: device is null");
        return nullptr;
    }
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
        spdlog::error("Cannot create NV12 texture: invalid geometry ({}x{})", width, height);
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = 0;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        spdlog::error("Failed to create NV12 texture ({}x{}): HRESULT {:#x}",
                      width, height, static_cast<unsigned long>(hr));
        return nullptr;
    }

    spdlog::debug("Created NV12 texture ({}x{})", width, height);
    return texture;
}

ID3D11Texture2D* TextureManager::create_p010_texture(int width, int height) {
    if (!device_) {
        spdlog::error("Cannot create P010 texture: device is null");
        return nullptr;
    }
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
        spdlog::error("Cannot create P010 texture: invalid geometry ({}x{})", width, height);
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_P010;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = 0;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        spdlog::error("Failed to create P010 texture ({}x{}): HRESULT {:#x}",
                      width, height, static_cast<unsigned long>(hr));
        return nullptr;
    }

    spdlog::debug("Created P010 texture ({}x{})", width, height);
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
    if (width <= 0 || height <= 0 || stride < width * 4) {
        spdlog::error("upload_data: invalid geometry (width={}, height={}, stride={})",
                      width, height, stride);
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

bool TextureManager::upload_plane_data(ID3D11Texture2D* texture, const uint8_t* data,
                                       int width, int height, int stride,
                                       bool is_16bit) {
    if (!texture || !data || !context_) {
        spdlog::error("upload_plane_data: invalid arguments (texture={}, data={}, context={})",
                      static_cast<void*>(texture), static_cast<const void*>(data),
                      static_cast<void*>(context_));
        return false;
    }
    const int bytes_per_sample = is_16bit ? 2 : 1;
    const int row_bytes = width * bytes_per_sample;
    if (width <= 0 || height <= 0 || stride < row_bytes) {
        spdlog::error("upload_plane_data: invalid geometry (width={}, height={}, stride={}, is_16bit={})",
                      width, height, stride, is_16bit);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    const DXGI_FORMAT expected_format = is_16bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    if (desc.Format != expected_format ||
        desc.Width != static_cast<UINT>(width) ||
        desc.Height != static_cast<UINT>(height)) {
        spdlog::error("upload_plane_data: texture mismatch (texture={}x{} fmt={}, "
                      "data={}x{}, expected_fmt={})",
                      desc.Width, desc.Height, static_cast<int>(desc.Format),
                      width, height, static_cast<int>(expected_format));
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context_->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        spdlog::error("Failed to map plane texture for upload: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    for (int y = 0; y < height; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    data + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(row_bytes));
    }

    context_->Unmap(texture, 0);
    spdlog::trace("Uploaded {} plane texture data ({}x{}, stride={})",
                  is_16bit ? "R16" : "R8", width, height, stride);
    return true;
}

bool TextureManager::upload_nv12_data(ID3D11Texture2D* texture, const uint8_t* data,
                                      int width, int height,
                                      int y_stride, int uv_stride,
                                      bool is_p010) {
    if (!texture || !data || !context_) {
        spdlog::error("upload_nv12_data: invalid arguments (texture={}, data={}, context={})",
                      static_cast<void*>(texture), static_cast<const void*>(data),
                      static_cast<void*>(context_));
        return false;
    }
    const int bytes_per_component = is_p010 ? 2 : 1;
    const int row_bytes = width * bytes_per_component;
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0 ||
        y_stride < row_bytes || uv_stride < row_bytes) {
        spdlog::error("upload_nv12_data: invalid geometry (width={}, height={}, "
                      "y_stride={}, uv_stride={}, is_p010={})",
                      width, height, y_stride, uv_stride, is_p010);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    const DXGI_FORMAT expected_format = is_p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    if (desc.Format != expected_format ||
        desc.Width != static_cast<UINT>(width) ||
        desc.Height != static_cast<UINT>(height)) {
        spdlog::error("upload_nv12_data: texture mismatch (texture={}x{} fmt={}, "
                      "data={}x{}, expected_fmt={})",
                      desc.Width, desc.Height, static_cast<int>(desc.Format),
                      width, height, static_cast<int>(expected_format));
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context_->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        spdlog::error("Failed to map NV12 texture for upload: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    const uint8_t* src_y = data;
    const uint8_t* src_uv = data + static_cast<size_t>(y_stride) * height;
    uint8_t* dst_y = static_cast<uint8_t*>(mapped.pData);
    uint8_t* dst_uv = dst_y + static_cast<size_t>(mapped.RowPitch) * height;
    const size_t copy_row_bytes = static_cast<size_t>(row_bytes);

    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_y + static_cast<size_t>(y) * mapped.RowPitch,
                    src_y + static_cast<size_t>(y) * y_stride,
                    copy_row_bytes);
    }
    for (int y = 0; y < height / 2; ++y) {
        std::memcpy(dst_uv + static_cast<size_t>(y) * mapped.RowPitch,
                    src_uv + static_cast<size_t>(y) * uv_stride,
                    copy_row_bytes);
    }

    context_->Unmap(texture, 0);
    spdlog::trace("Uploaded {} texture data ({}x{}, y_stride={}, uv_stride={})",
                  is_p010 ? "P010" : "NV12", width, height, y_stride, uv_stride);
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

bool TextureManager::create_nv12_plane_srvs(
    ID3D11Texture2D* texture,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& y_srv,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& uv_srv) {
    y_srv.Reset();
    uv_srv.Reset();
    if (!device_ || !texture) {
        spdlog::error("create_nv12_plane_srvs: invalid arguments (device={}, texture={})",
                      static_cast<void*>(device_), static_cast<void*>(texture));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    DXGI_FORMAT y_format = DXGI_FORMAT_R8_UNORM;
    DXGI_FORMAT uv_format = DXGI_FORMAT_R8G8_UNORM;
    if (desc.Format == DXGI_FORMAT_P010 || desc.Format == DXGI_FORMAT_P016) {
        y_format = DXGI_FORMAT_R16_UNORM;
        uv_format = DXGI_FORMAT_R16G16_UNORM;
    } else if (desc.Format != DXGI_FORMAT_NV12) {
        spdlog::warn("Creating planar SRVs for unexpected format={}",
                     static_cast<int>(desc.Format));
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC y_desc = {};
    y_desc.Format = y_format;
    y_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    y_desc.Texture2D.MipLevels = 1;
    HRESULT hr = device_->CreateShaderResourceView(texture, &y_desc, &y_srv);
    if (FAILED(hr)) {
        spdlog::error("Failed to create NV12 Y SRV: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC uv_desc = {};
    uv_desc.Format = uv_format;
    uv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uv_desc.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(texture, &uv_desc, &uv_srv);
    if (FAILED(hr)) {
        spdlog::error("Failed to create NV12 UV SRV: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        y_srv.Reset();
        return false;
    }
    return true;
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

    if (!create_nv12_plane_srvs(copy_texture.Get(), y_srv, uv_srv)) {
        copy_texture.Reset();
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
