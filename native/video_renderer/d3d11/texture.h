#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace vr {

class TextureManager {
public:
    TextureManager(ID3D11Device* device, ID3D11DeviceContext* context);
    ~TextureManager() = default;

    ID3D11Texture2D* create_rgba_texture(int width, int height);
    ID3D11Texture2D* create_nv12_texture(int width, int height);
    ID3D11Texture2D* create_p010_texture(int width, int height);
    bool upload_data(ID3D11Texture2D* texture, const uint8_t* data, int width, int height, int stride);
    bool upload_nv12_data(ID3D11Texture2D* texture, const uint8_t* data,
                          int width, int height, int y_stride, int uv_stride,
                          bool is_p010 = false);
    ID3D11ShaderResourceView* create_srv(ID3D11Texture2D* texture);
    bool create_nv12_plane_srvs(
        ID3D11Texture2D* texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& y_srv,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& uv_srv);
    bool open_shared_texture(ID3D11Texture2D* source,
                             Microsoft::WRL::ComPtr<ID3D11Texture2D>& opened);
    bool ensure_nv12_copy_resources(
        ID3D11Texture2D* source,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& copy_texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& y_srv,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& uv_srv,
        bool* created_new = nullptr);

private:
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
};

} // namespace vr
