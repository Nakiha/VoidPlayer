#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string>
#include <wrl/client.h>

namespace vr {

struct CompiledShader {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer;
};

class ShaderManager {
public:
    explicit ShaderManager(ID3D11Device* device);
    ~ShaderManager() = default;

    bool compile_from_source(const std::string& source,
                             const std::string& vs_entry, const std::string& ps_entry,
                             CompiledShader& out);

    bool create_input_layout(ID3D11Device* device, ID3DBlob* vs_blob, CompiledShader& out);
    bool create_constant_buffer(ID3D11Device* device, UINT size, CompiledShader& out);

private:
    bool compile_stage(const std::string& source, const std::string& entry,
                       const std::string& target, ID3DBlob** out_blob);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
};

} // namespace vr
