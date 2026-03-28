#pragma once
#include <d3d11.h>
#include <cstdint>

namespace vr {

class TextureManager {
public:
    TextureManager(ID3D11Device* device, ID3D11DeviceContext* context);
    ~TextureManager() = default;

    ID3D11Texture2D* create_rgba_texture(int width, int height);
    bool upload_data(ID3D11Texture2D* texture, const uint8_t* data, int width, int height, int stride);
    ID3D11ShaderResourceView* create_srv(ID3D11Texture2D* texture);

private:
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
};

} // namespace vr
