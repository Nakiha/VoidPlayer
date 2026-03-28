#include "texture.h"
#include <cstring>
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

} // namespace vr
