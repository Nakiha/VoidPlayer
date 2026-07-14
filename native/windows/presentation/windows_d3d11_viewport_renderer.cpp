#include "windows/presentation/windows_d3d11_viewport_renderer.h"

#include "renderer/renderer_limits.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <spdlog/spdlog.h>

namespace vr {
namespace {

constexpr char kViewportShader[] = R"hlsl(
struct VSInput { float2 position : POSITION; float2 texcoord : TEXCOORD0; };
struct VSOutput { float4 position : SV_POSITION; float2 texcoord : TEXCOORD0; };

#define MODE_SPLIT_SCREEN 1
#define COLOR_RANGE_FULL 2
#define COLOR_MATRIX_UNKNOWN 0
#define COLOR_MATRIX_BT601 1
#define COLOR_MATRIX_BT709 2
#define COLOR_MATRIX_BT2020_NCL 3
#define COLOR_TRANSFER_SDR 1
#define COLOR_TRANSFER_PQ 2
#define COLOR_TRANSFER_HLG 3
#define COLOR_PRIMARIES_BT2020 3
#define OUTPUT_TARGET_WINDOWS_SCRGB 1

Texture2D u_rgba[4] : register(t0);
Texture2D<float> u_y[4] : register(t4);
Texture2D<float2> u_uv[4] : register(t8);
Texture2D<float> u_u[4] : register(t12);
Texture2D<float> u_v[4] : register(t16);
SamplerState u_sampler : register(s0);

cbuffer Constants : register(b0) {
  int u_mode;
  int u_track_count;
  float u_split_pos;
  float u_zoom_ratio;
  float u_canvas_width;
  float u_canvas_height;
  float2 u_view_offset;
  int4 u_order;
  float4 u_video_aspect;
  int u_nv12_mask;
  int u_planar_yuv_mask;
  float2 u_pad1;
  float4 u_nv12_uv_scale_y;
  float4 u_nv12_uv_scale_x;
  float4 u_track_scale;
  float4 u_display_offset_x;
  float4 u_display_offset_y;
  float4 u_inv_display_size_x;
  float4 u_inv_display_size_y;
  float4 u_view_offset_uv_x;
  float4 u_view_offset_uv_y;
  float4 u_background_color;
  int4 u_color_range;
  int4 u_color_matrix;
  int4 u_color_transfer;
  int4 u_color_primaries;
  int u_output_target;
  float u_sdr_white_scale;
  float2 u_pad2;
};

float3 linear_to_srgb(float3 x) {
  x = max(x, 0.0);
  return lerp(x * 12.92,
              1.055 * pow(x, 1.0 / 2.4) - 0.055,
              step(0.0031308, x));
}

float3 srgb_to_linear(float3 x) {
  x = saturate(x);
  return lerp(x / 12.92,
              pow((x + 0.055) / 1.055, 2.4),
              step(0.04045, x));
}

float3 primaries_to_bt709(float3 rgb, int primaries) {
  if (primaries != COLOR_PRIMARIES_BT2020) return rgb;
  return float3(1.6605 * rgb.r - 0.5876 * rgb.g - 0.0728 * rgb.b,
               -0.1246 * rgb.r + 1.1329 * rgb.g - 0.0083 * rgb.b,
               -0.0182 * rgb.r - 0.1006 * rgb.g + 1.1187 * rgb.b);
}

float3 pq_to_linear_nits(float3 x) {
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  float3 p = pow(saturate(x), 1.0 / m2);
  return pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1e-6), 1.0 / m1) * 10000.0;
}

float3 hlg_to_linear(float3 x) {
  const float a = 0.17883277;
  const float b = 0.28466892;
  const float c = 0.55991073;
  x = saturate(x);
  return lerp((x * x) / 3.0,
              (exp((x - c) / a) + b) / 12.0,
              step(0.5, x));
}

float3 map_to_output(float3 rgb, int transfer, int primaries) {
  if (u_output_target == OUTPUT_TARGET_WINDOWS_SCRGB) {
    if (transfer == COLOR_TRANSFER_PQ) {
      return primaries_to_bt709(pq_to_linear_nits(rgb) / 80.0, primaries);
    }
    if (transfer == COLOR_TRANSFER_HLG) {
      return primaries_to_bt709(hlg_to_linear(rgb) * (4.0 * 203.0 / 80.0), primaries);
    }
    return primaries_to_bt709(srgb_to_linear(rgb), primaries) * u_sdr_white_scale;
  }
  if (transfer == COLOR_TRANSFER_PQ) {
    float3 linear_rgb = primaries_to_bt709(pq_to_linear_nits(rgb) / 203.0, primaries);
    return saturate(linear_to_srgb(linear_rgb / (1.0 + linear_rgb)));
  }
  if (transfer == COLOR_TRANSFER_HLG) {
    float3 linear_rgb = primaries_to_bt709(hlg_to_linear(rgb) * 4.0, primaries);
    return saturate(linear_to_srgb(linear_rgb / (1.0 + linear_rgb)));
  }
  if (primaries == COLOR_PRIMARIES_BT2020) {
    return saturate(linear_to_srgb(primaries_to_bt709(srgb_to_linear(rgb), primaries)));
  }
  return saturate(rgb);
}

float4 background_color() {
  float3 rgb = u_background_color.rgb;
  if (u_output_target == OUTPUT_TARGET_WINDOWS_SCRGB) {
    rgb = srgb_to_linear(saturate(rgb)) * u_sdr_white_scale;
  }
  return float4(rgb, 1.0);
}

float y_at(int slot, float2 uv) {
  if (slot == 0) return u_y[0].Sample(u_sampler, uv);
  if (slot == 1) return u_y[1].Sample(u_sampler, uv);
  if (slot == 2) return u_y[2].Sample(u_sampler, uv);
  return u_y[3].Sample(u_sampler, uv);
}
float2 uv_at(int slot, float2 uv) {
  if (slot == 0) return u_uv[0].Sample(u_sampler, uv);
  if (slot == 1) return u_uv[1].Sample(u_sampler, uv);
  if (slot == 2) return u_uv[2].Sample(u_sampler, uv);
  return u_uv[3].Sample(u_sampler, uv);
}
float u_at(int slot, float2 uv) {
  if (slot == 0) return u_u[0].Sample(u_sampler, uv);
  if (slot == 1) return u_u[1].Sample(u_sampler, uv);
  if (slot == 2) return u_u[2].Sample(u_sampler, uv);
  return u_u[3].Sample(u_sampler, uv);
}
float v_at(int slot, float2 uv) {
  if (slot == 0) return u_v[0].Sample(u_sampler, uv);
  if (slot == 1) return u_v[1].Sample(u_sampler, uv);
  if (slot == 2) return u_v[2].Sample(u_sampler, uv);
  return u_v[3].Sample(u_sampler, uv);
}
float4 rgba_at(int slot, float2 uv) {
  if (slot == 0) return u_rgba[0].Sample(u_sampler, uv);
  if (slot == 1) return u_rgba[1].Sample(u_sampler, uv);
  if (slot == 2) return u_rgba[2].Sample(u_sampler, uv);
  return u_rgba[3].Sample(u_sampler, uv);
}

float3 yuv_to_rgb(float y, float2 uv, int slot) {
  float y_full = y;
  float2 cbcr = (uv * 255.0 - 128.0) / 255.0;
  if (u_color_range[slot] != COLOR_RANGE_FULL) {
    y_full = (y * 255.0 - 16.0) / 219.0;
    cbcr = (uv * 255.0 - 128.0) / 224.0;
  }
  float cb = cbcr.x;
  float cr = cbcr.y;
  float3 rgb;
  if (u_color_matrix[slot] == COLOR_MATRIX_BT2020_NCL) {
    rgb = float3(y_full + 1.4746 * cr,
                 y_full - 0.164553 * cb - 0.571353 * cr,
                 y_full + 1.8814 * cb);
  } else if (u_color_matrix[slot] == COLOR_MATRIX_BT709 ||
             u_color_matrix[slot] == COLOR_MATRIX_UNKNOWN) {
    rgb = float3(y_full + 1.5748 * cr,
                 y_full - 0.187324 * cb - 0.468124 * cr,
                 y_full + 1.8556 * cb);
  } else {
    rgb = float3(y_full + 1.402 * cr,
                 y_full - 0.344136 * cb - 0.714136 * cr,
                 y_full + 1.772 * cb);
  }
  if (u_color_transfer[slot] == COLOR_TRANSFER_SDR) rgb -= 1.0 / 255.0;
  return map_to_output(rgb, u_color_transfer[slot], u_color_primaries[slot]);
}

float4 sample_track(int slot, float2 uv) {
  if ((u_nv12_mask & (1 << slot)) != 0) {
    float2 scaled = uv * float2(u_nv12_uv_scale_x[slot], u_nv12_uv_scale_y[slot]);
    return float4(yuv_to_rgb(y_at(slot, scaled), uv_at(slot, scaled), slot), 1.0);
  }
  if ((u_planar_yuv_mask & (1 << slot)) != 0) {
    return float4(yuv_to_rgb(y_at(slot, uv), float2(u_at(slot, uv), v_at(slot, uv)), slot), 1.0);
  }
  float4 color = rgba_at(slot, uv);
  color.rgb = map_to_output(color.rgb,
                            u_color_transfer[slot],
                            u_color_primaries[slot]);
  color.a = 1.0;
  return color;
}

VSOutput VSMain(VSInput input) {
  VSOutput output;
  output.position = float4(input.position, 0.0, 1.0);
  output.texcoord = input.texcoord;
  return output;
}

float4 PSMain(VSOutput input) : SV_TARGET {
  int slot;
  float2 local_uv = input.texcoord;
  if (u_mode == MODE_SPLIT_SCREEN) {
    slot = input.texcoord.x < u_split_pos ? u_order[0] : u_order[1];
  } else {
    int count = max(u_track_count, 1);
    float scaled_x = input.texcoord.x * float(count);
    int display_slot = clamp(int(scaled_x), 0, count - 1);
    slot = u_order[display_slot];
    local_uv.x = scaled_x - float(display_slot);
  }
  slot = clamp(slot, 0, 3);
  if (u_video_aspect[slot] <= 0.0) return background_color();

  float2 offset = float2(u_display_offset_x[slot], u_display_offset_y[slot]);
  float2 inverse_size = float2(u_inv_display_size_x[slot], u_inv_display_size_y[slot]);
  float2 pan = float2(u_view_offset_uv_x[slot], u_view_offset_uv_y[slot]);
  float2 source_uv = (local_uv - offset) * inverse_size - pan;
  if (source_uv.x < 0.0 || source_uv.x > 1.0 ||
      source_uv.y < 0.0 || source_uv.y > 1.0) {
    return background_color();
  }

  float4 color = sample_track(slot, source_uv);
  if (u_mode == MODE_SPLIT_SCREEN && u_canvas_width > 0.0) {
    float distance = abs(input.texcoord.x * u_canvas_width - u_split_pos * u_canvas_width);
    if (distance <= 2.0) {
      float alpha = distance <= 1.25 ? 1.0 : 1.0 - ((distance - 1.25) / 0.75);
      color.rgb = lerp(color.rgb, 1.0 - color.rgb, alpha);
    }
  }
  color.a = 1.0;
  return color;
}
)hlsl";

constexpr char kOverlayShader[] = R"hlsl(
struct VSInput { float2 position : POSITION; float4 color : COLOR0; };
struct VSOutput { float4 position : SV_POSITION; float4 color : COLOR0; };
cbuffer OverlayConstants : register(b0) {
  int u_output_target;
  float u_sdr_white_scale;
  float2 u_padding;
};
float3 srgb_to_linear(float3 x) {
  x = saturate(x);
  return lerp(x / 12.92,
              pow((x + 0.055) / 1.055, 2.4),
              step(0.04045, x));
}
VSOutput VSOverlay(VSInput input) {
  VSOutput output;
  output.position = float4(input.position, 0.0, 1.0);
  output.color = input.color;
  return output;
}
float4 PSOverlay(VSOutput input) : SV_TARGET {
  float3 rgb = input.color.rgb;
  if (u_output_target == 1) {
    rgb = srgb_to_linear(rgb) * u_sdr_white_scale;
  }
  return float4(rgb, input.color.a);
}
)hlsl";

struct OverlayConstants {
  int output_target = 0;
  float sdr_white_scale = 1.0f;
  float padding[2] = {};
};

struct Vertex {
  float position[2];
  float texcoord[2];
};

constexpr Vertex kFullscreenVertices[] = {
    {{-1.0f, -1.0f}, {0.0f, 1.0f}},
    {{-1.0f, 1.0f}, {0.0f, 0.0f}},
    {{1.0f, -1.0f}, {1.0f, 1.0f}},
    {{1.0f, 1.0f}, {1.0f, 0.0f}},
};

bool valid_dimensions(int width, int height) {
  return width > 0 && height > 0 && width <= kMaxRendererDimension &&
         height <= kMaxRendererDimension;
}

bool is_supported_hardware_format(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010 ||
         format == DXGI_FORMAT_P016;
}

DXGI_FORMAT y_plane_format(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8_UNORM
                                    : DXGI_FORMAT_R16_UNORM;
}

DXGI_FORMAT uv_plane_format(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8G8_UNORM
                                    : DXGI_FORMAT_R16G16_UNORM;
}

}  // namespace

bool WindowsD3D11ViewportRenderer::initialize(ID3D11Device* device,
                                              ID3D11DeviceContext* context) {
  shutdown();
  if (!device || !context) {
    return fail("viewport renderer requires a D3D11 device and context");
  }
  device_ = device;
  context_ = context;
  return create_pipeline() && create_overlay_pipeline();
}

void WindowsD3D11ViewportRenderer::shutdown() {
  unbind_shader_resources();
  tracks_ = {};
  sampler_.Reset();
  overlay_blend_state_.Reset();
  overlay_constant_buffer_.Reset();
  overlay_vertex_buffer_.Reset();
  overlay_input_layout_.Reset();
  overlay_pixel_shader_.Reset();
  overlay_vertex_shader_.Reset();
  overlay_vertex_capacity_ = 0;
  constant_buffer_.Reset();
  vertex_buffer_.Reset();
  input_layout_.Reset();
  pixel_shader_.Reset();
  vertex_shader_.Reset();
  context_.Reset();
  device_.Reset();
  stats_ = {};
  last_layout_revision_ = 0;
  layout_log_count_ = 0;
  last_error_.clear();
}

bool WindowsD3D11ViewportRenderer::create_pipeline() {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
  flags |= D3DCOMPILE_DEBUG;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> pixel_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  HRESULT result = D3DCompile(kViewportShader,
                              sizeof(kViewportShader) - 1,
                              "windows_d3d11_viewport",
                              nullptr,
                              nullptr,
                              "VSMain",
                              "vs_5_0",
                              flags,
                              0,
                              &vertex_blob,
                              &errors);
  if (FAILED(result)) {
    const std::string message = errors
        ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                      errors->GetBufferSize())
        : "unknown vertex shader error";
    return fail("viewport vertex shader compilation failed: " + message);
  }
  errors.Reset();
  result = D3DCompile(kViewportShader,
                      sizeof(kViewportShader) - 1,
                      "windows_d3d11_viewport",
                      nullptr,
                      nullptr,
                      "PSMain",
                      "ps_5_0",
                      flags,
                      0,
                      &pixel_blob,
                      &errors);
  if (FAILED(result)) {
    const std::string message = errors
        ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                      errors->GetBufferSize())
        : "unknown pixel shader error";
    return fail("viewport pixel shader compilation failed: " + message);
  }
  result = device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                       vertex_blob->GetBufferSize(),
                                       nullptr,
                                       &vertex_shader_);
  if (FAILED(result)) {
    return fail("could not create viewport vertex shader");
  }
  result = device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                      pixel_blob->GetBufferSize(),
                                      nullptr,
                                      &pixel_shader_);
  if (FAILED(result)) {
    return fail("could not create viewport pixel shader");
  }
  const D3D11_INPUT_ELEMENT_DESC input_desc[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  result = device_->CreateInputLayout(input_desc,
                                      ARRAYSIZE(input_desc),
                                      vertex_blob->GetBufferPointer(),
                                      vertex_blob->GetBufferSize(),
                                      &input_layout_);
  if (FAILED(result)) {
    return fail("could not create viewport input layout");
  }

  D3D11_BUFFER_DESC vertex_desc = {};
  vertex_desc.ByteWidth = sizeof(kFullscreenVertices);
  vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
  vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA vertex_data = {};
  vertex_data.pSysMem = kFullscreenVertices;
  result = device_->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer_);
  if (FAILED(result)) {
    return fail("could not create viewport vertex buffer");
  }

  D3D11_BUFFER_DESC constant_desc = {};
  constant_desc.ByteWidth = static_cast<UINT>(sizeof(ShaderConstants));
  constant_desc.Usage = D3D11_USAGE_DEFAULT;
  constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  result = device_->CreateBuffer(&constant_desc, nullptr, &constant_buffer_);
  if (FAILED(result)) {
    return fail("could not create viewport constant buffer");
  }

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  result = device_->CreateSamplerState(&sampler_desc, &sampler_);
  if (FAILED(result)) {
    return fail("could not create viewport sampler");
  }
  last_error_.clear();
  return true;
}

bool WindowsD3D11ViewportRenderer::create_overlay_pipeline() {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
  flags |= D3DCOMPILE_DEBUG;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> pixel_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  HRESULT result = D3DCompile(kOverlayShader, sizeof(kOverlayShader) - 1,
                              "windows_d3d11_analysis_overlay", nullptr, nullptr,
                              "VSOverlay", "vs_5_0", flags, 0,
                              &vertex_blob, &errors);
  if (FAILED(result)) {
    const std::string message = errors
        ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                      errors->GetBufferSize())
        : "unknown vertex shader error";
    return fail("overlay vertex shader compilation failed: " + message);
  }
  errors.Reset();
  result = D3DCompile(kOverlayShader, sizeof(kOverlayShader) - 1,
                      "windows_d3d11_analysis_overlay", nullptr, nullptr,
                      "PSOverlay", "ps_5_0", flags, 0, &pixel_blob, &errors);
  if (FAILED(result)) {
    const std::string message = errors
        ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                      errors->GetBufferSize())
        : "unknown pixel shader error";
    return fail("overlay pixel shader compilation failed: " + message);
  }
  if (FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                          vertex_blob->GetBufferSize(), nullptr,
                                          &overlay_vertex_shader_)) ||
      FAILED(device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                         pixel_blob->GetBufferSize(), nullptr,
                                         &overlay_pixel_shader_))) {
    return fail("could not create analysis overlay shaders");
  }
  const D3D11_INPUT_ELEMENT_DESC input_desc[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (FAILED(device_->CreateInputLayout(input_desc, ARRAYSIZE(input_desc),
                                         vertex_blob->GetBufferPointer(),
                                         vertex_blob->GetBufferSize(),
                                         &overlay_input_layout_))) {
    return fail("could not create analysis overlay input layout");
  }
  D3D11_BUFFER_DESC constant_desc = {};
  constant_desc.ByteWidth = sizeof(OverlayConstants);
  constant_desc.Usage = D3D11_USAGE_DEFAULT;
  constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(device_->CreateBuffer(&constant_desc, nullptr,
                                    &overlay_constant_buffer_))) {
    return fail("could not create analysis overlay constant buffer");
  }
  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device_->CreateBlendState(&blend_desc, &overlay_blend_state_))) {
    return fail("could not create analysis overlay blend state");
  }
  return true;
}

bool WindowsD3D11ViewportRenderer::ensure_overlay_vertex_buffer(
    size_t vertex_count) {
  if (overlay_vertex_buffer_ && vertex_count <= overlay_vertex_capacity_) {
    return true;
  }
  size_t capacity = std::max<size_t>(256, overlay_vertex_capacity_);
  while (capacity < vertex_count) capacity *= 2;
  if (capacity > std::numeric_limits<UINT>::max() /
                     sizeof(AnalysisOverlayGpuVertex)) {
    return fail("analysis overlay geometry exceeds D3D11 buffer limits");
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = static_cast<UINT>(capacity * sizeof(AnalysisOverlayGpuVertex));
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
  if (FAILED(device_->CreateBuffer(&desc, nullptr, &buffer))) {
    return fail("could not allocate analysis overlay vertex buffer");
  }
  overlay_vertex_buffer_ = std::move(buffer);
  overlay_vertex_capacity_ = capacity;
  return true;
}

bool WindowsD3D11ViewportRenderer::draw_overlay(
    const AnalysisOverlayPrimitivePackage& package,
    const PresentationSnapshot& presentation,
    ID3D11RenderTargetView* target,
    ColorOutputTarget output_target,
    double sdr_white_level_nits) {
  auto geometry = build_analysis_overlay_gpu_geometry(
      package, presentation.constants,
      static_cast<int>(std::lround(presentation.constants.canvas_width)),
      static_cast<int>(std::lround(presentation.constants.canvas_height)));
  stats_.overlay_last_vertex_count = geometry.vertices.size();
  stats_.overlay_last_fill_rect_count = geometry.fill_rect_count;
  stats_.overlay_last_line_rect_count = geometry.line_rect_count;
  if (geometry.empty()) return true;
  if (!target || !overlay_vertex_shader_ || !overlay_pixel_shader_ ||
      !ensure_overlay_vertex_buffer(geometry.vertices.size())) {
    ++stats_.overlay_failure_count;
    return false;
  }
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(context_->Map(overlay_vertex_buffer_.Get(), 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    ++stats_.overlay_failure_count;
    return fail("could not map analysis overlay vertex buffer");
  }
  std::memcpy(mapped.pData, geometry.vertices.data(),
              geometry.vertices.size() * sizeof(AnalysisOverlayGpuVertex));
  context_->Unmap(overlay_vertex_buffer_.Get(), 0);

  OverlayConstants constants;
  constants.output_target =
      output_target == ColorOutputTarget::kWindowsLinearScRGB ? 1 : 0;
  constants.sdr_white_scale = static_cast<float>(
      std::isfinite(sdr_white_level_nits) && sdr_white_level_nits > 0.0
          ? sdr_white_level_nits / 80.0
          : 1.0);
  context_->UpdateSubresource(overlay_constant_buffer_.Get(), 0, nullptr,
                              &constants, 0, 0);
  context_->OMSetRenderTargets(1, &target, nullptr);
  const float blend_factor[4] = {};
  context_->OMSetBlendState(overlay_blend_state_.Get(), blend_factor, 0xffffffffu);
  const UINT stride = sizeof(AnalysisOverlayGpuVertex);
  const UINT offset = 0;
  ID3D11Buffer* vertices = overlay_vertex_buffer_.Get();
  context_->IASetInputLayout(overlay_input_layout_.Get());
  context_->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(overlay_vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(overlay_pixel_shader_.Get(), nullptr, 0);
  ID3D11Buffer* constant_buffer = overlay_constant_buffer_.Get();
  context_->VSSetConstantBuffers(0, 1, &constant_buffer);
  context_->PSSetConstantBuffers(0, 1, &constant_buffer);
  context_->Draw(static_cast<UINT>(geometry.vertices.size()), 0);
  context_->OMSetBlendState(nullptr, blend_factor, 0xffffffffu);
  ID3D11RenderTargetView* null_target = nullptr;
  context_->OMSetRenderTargets(1, &null_target, nullptr);
  ++stats_.overlay_draw_count;
  return true;
}

bool WindowsD3D11ViewportRenderer::draw(
    const RendererDrawSnapshot& draw_snapshot,
    const PresentationSnapshot& presentation,
    ID3D11RenderTargetView* target,
    ColorOutputTarget output_target,
    double sdr_white_level_nits) {
  if (!device_ || !context_ || !target || !vertex_shader_ || !pixel_shader_) {
    return fail("viewport renderer is not initialized");
  }
  if (!valid_dimensions(draw_snapshot.target_width,
                        draw_snapshot.target_height)) {
    return fail("viewport renderer received invalid target dimensions");
  }
  Microsoft::WRL::ComPtr<ID3D11Resource> target_resource;
  target->GetResource(&target_resource);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> target_texture;
  if (!target_resource || FAILED(target_resource.As(&target_texture)) ||
      !target_texture) {
    return fail("viewport target is not a D3D11 texture");
  }
  D3D11_TEXTURE2D_DESC target_desc = {};
  target_texture->GetDesc(&target_desc);
  const DXGI_FORMAT expected_format =
      output_target == ColorOutputTarget::kWindowsLinearScRGB
          ? DXGI_FORMAT_R16G16B16A16_FLOAT
          : DXGI_FORMAT_B8G8R8A8_UNORM;
  if (target_desc.Width != static_cast<UINT>(draw_snapshot.target_width) ||
      target_desc.Height != static_cast<UINT>(draw_snapshot.target_height) ||
      target_desc.Format != expected_format) {
    return fail("viewport target dimensions or format do not match output");
  }

  rgba_srvs_.fill(nullptr);
  y_srvs_.fill(nullptr);
  uv_srvs_.fill(nullptr);
  u_srvs_.fill(nullptr);
  v_srvs_.fill(nullptr);
  ShaderConstants constants = presentation.constants;
  if (draw_snapshot.layout_revision != last_layout_revision_) {
    last_layout_revision_ = draw_snapshot.layout_revision;
    ++layout_log_count_;
    if (layout_log_count_ <= 12 || layout_log_count_ % 60 == 0) {
      spdlog::info(
          "[WindowsLayout] d3d11 draw={} layout_rev={} target={}x{} mode={} "
          "zoom={:.4f} offset=({:.1f},{:.1f}) split={:.4f}",
          stats_.draw_count + 1, draw_snapshot.layout_revision,
          draw_snapshot.target_width, draw_snapshot.target_height,
          draw_snapshot.layout.mode, draw_snapshot.layout.zoom_ratio,
          draw_snapshot.layout.view_offset[0],
          draw_snapshot.layout.view_offset[1],
          draw_snapshot.layout.split_pos);
    }
  }
  constants.nv12_mask = 0;
  constants.planar_yuv_mask = 0;
  constants.output_target =
      output_target == ColorOutputTarget::kWindowsLinearScRGB ? 1 : 0;
  constants.sdr_white_scale = static_cast<float>(
      std::isfinite(sdr_white_level_nits) && sdr_white_level_nits > 0.0
          ? sdr_white_level_nits / 80.0
          : 1.0);

  for (size_t slot = 0; slot < kMaxTracks; ++slot) {
    const bool matches = draw_snapshot.tracks[slot].active &&
        draw_snapshot.decision.frames[slot].has_value() &&
        draw_snapshot.decision.file_ids[slot] ==
            draw_snapshot.tracks[slot].file_id &&
        draw_snapshot.decision.track_generations[slot] ==
            draw_snapshot.tracks[slot].generation;
    if (!matches) {
      clear_track_bindings(slot, constants);
      continue;
    }
    if (!prepare_track(slot,
                       draw_snapshot.decision.frames[slot].value(),
                       constants)) {
      unbind_shader_resources();
      return false;
    }
  }

  const float opaque_clear[4] = {
      draw_snapshot.background_color[0],
      draw_snapshot.background_color[1],
      draw_snapshot.background_color[2],
      1.0f,
  };
  context_->ClearRenderTargetView(target, opaque_clear);
  context_->OMSetRenderTargets(1, &target, nullptr);
  D3D11_VIEWPORT viewport = {};
  viewport.Width = static_cast<float>(draw_snapshot.target_width);
  viewport.Height = static_cast<float>(draw_snapshot.target_height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context_->RSSetViewports(1, &viewport);

  const UINT stride = sizeof(Vertex);
  const UINT offset = 0;
  ID3D11Buffer* vertex_buffer = vertex_buffer_.Get();
  context_->IASetInputLayout(input_layout_.Get());
  context_->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
  context_->UpdateSubresource(constant_buffer_.Get(), 0, nullptr, &constants, 0, 0);
  ID3D11Buffer* constant_buffer = constant_buffer_.Get();
  context_->VSSetConstantBuffers(0, 1, &constant_buffer);
  context_->PSSetConstantBuffers(0, 1, &constant_buffer);
  ID3D11SamplerState* sampler = sampler_.Get();
  context_->PSSetSamplers(0, 1, &sampler);
  context_->PSSetShaderResources(0, static_cast<UINT>(kMaxTracks), rgba_srvs_.data());
  context_->PSSetShaderResources(4, static_cast<UINT>(kMaxTracks), y_srvs_.data());
  context_->PSSetShaderResources(8, static_cast<UINT>(kMaxTracks), uv_srvs_.data());
  context_->PSSetShaderResources(12, static_cast<UINT>(kMaxTracks), u_srvs_.data());
  context_->PSSetShaderResources(16, static_cast<UINT>(kMaxTracks), v_srvs_.data());
  context_->Draw(4, 0);
  unbind_shader_resources();
  ID3D11RenderTargetView* null_target = nullptr;
  context_->OMSetRenderTargets(1, &null_target, nullptr);
  ++stats_.draw_count;
  last_error_.clear();
  return true;
}

bool WindowsD3D11ViewportRenderer::prepare_track(
    size_t slot,
    const TextureFrame& frame,
    ShaderConstants& constants) {
  if (const auto* storage = frame.windows_d3d11_storage()) {
    return prepare_hardware_track(slot, frame, *storage, constants);
  }
  if (const auto* storage = frame.cpu_nv12_storage()) {
    ++stats_.software_frame_count;
    return prepare_cpu_nv12_track(slot, *storage, constants);
  }
  if (const auto* storage = frame.cpu_planar_yuv_storage()) {
    ++stats_.software_frame_count;
    return prepare_cpu_planar_track(slot, *storage, constants);
  }
  if (const auto* storage = frame.cpu_rgba_storage()) {
    ++stats_.software_frame_count;
    return prepare_cpu_rgba_track(slot, frame, *storage, constants);
  }
  return fail("viewport renderer received unsupported frame storage");
}

bool WindowsD3D11ViewportRenderer::prepare_hardware_track(
    size_t slot,
    const TextureFrame& frame,
    const WindowsD3D11FrameStorage& storage,
    ShaderConstants& constants) {
  if (!storage.texture) {
    return fail("D3D11 frame storage has no texture");
  }
  D3D11_TEXTURE2D_DESC source_desc = {};
  storage.texture->GetDesc(&source_desc);
  if (!is_supported_hardware_format(source_desc.Format) ||
      storage.array_index < 0 ||
      static_cast<UINT>(storage.array_index) >= source_desc.ArraySize) {
    return fail("D3D11 frame storage has unsupported format or array slice");
  }
  auto& resources = tracks_[slot];
  const bool same_frame_owner =
      storage.frame_ref && resources.cached_hardware_frame_ref &&
      !storage.frame_ref.owner_before(resources.cached_hardware_frame_ref) &&
      !resources.cached_hardware_frame_ref.owner_before(storage.frame_ref);
  const bool source_cache_hit =
      same_frame_owner && resources.hardware_copy &&
      resources.cached_hardware_source == storage.texture &&
      resources.cached_hardware_array_index == storage.array_index &&
      resources.cached_hardware_pts_us == frame.pts_us &&
      resources.cached_hardware_dts_us == frame.dts_us;
  if (source_cache_hit) {
    y_srvs_[slot] = resources.hardware_y_srv.Get();
    uv_srvs_[slot] = resources.hardware_uv_srv.Get();
    constants.nv12_mask |= (1 << static_cast<int>(slot));
    ++stats_.hardware_frame_count;
    ++stats_.source_frame_cache_hit_count;
    return true;
  }
  ++stats_.source_frame_cache_miss_count;
  Microsoft::WRL::ComPtr<ID3D11Device> source_device;
  storage.texture->GetDevice(&source_device);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> opened_source;
  ID3D11Texture2D* copy_source = storage.texture;
  if (source_device.Get() != device_.Get()) {
    Microsoft::WRL::ComPtr<IDXGIResource> shared_resource;
    if (FAILED(storage.texture->QueryInterface(IID_PPV_ARGS(&shared_resource))) ||
        !shared_resource) {
      return fail("cross-device D3D11 frame is not a shareable DXGI resource");
    }
    HANDLE shared_handle = nullptr;
    if (FAILED(shared_resource->GetSharedHandle(&shared_handle)) ||
        !shared_handle ||
        FAILED(device_->OpenSharedResource(
            shared_handle, IID_PPV_ARGS(&opened_source))) ||
        !opened_source) {
      return fail("could not open D3D11 frame on the presentation device");
    }
    copy_source = opened_source.Get();
    copy_source->GetDesc(&source_desc);
  }
  if (!ensure_hardware_copy(resources, copy_source)) {
    return false;
  }
  const UINT source_subresource = D3D11CalcSubresource(
      0, static_cast<UINT>(storage.array_index), 1);
  context_->CopySubresourceRegion(resources.hardware_copy.Get(),
                                  0,
                                  0,
                                  0,
                                  0,
                                  copy_source,
                                  source_subresource,
                                  nullptr);
  resources.cached_hardware_frame_ref = storage.frame_ref;
  resources.cached_hardware_source = storage.texture;
  resources.cached_hardware_array_index = storage.array_index;
  resources.cached_hardware_pts_us = frame.pts_us;
  resources.cached_hardware_dts_us = frame.dts_us;
  y_srvs_[slot] = resources.hardware_y_srv.Get();
  uv_srvs_[slot] = resources.hardware_uv_srv.Get();
  constants.nv12_mask |= (1 << static_cast<int>(slot));
  ++stats_.hardware_frame_count;
  ++stats_.video_source_update_count;
  return true;
}

bool WindowsD3D11ViewportRenderer::prepare_cpu_rgba_track(
    size_t slot,
    const TextureFrame& frame,
    const CpuRgbaFrameStorage& storage,
    ShaderConstants& constants) {
  if (!storage.data || !valid_dimensions(frame.width, frame.height) ||
      storage.stride < frame.width * 4) {
    return fail("CPU BGRA frame storage is invalid");
  }
  auto& resource = tracks_[slot].rgba;
  if (!ensure_plane(resource,
                    frame.width,
                    frame.height,
                    DXGI_FORMAT_B8G8R8A8_UNORM) ||
      !upload_plane(resource, storage.data->data(), storage.stride, 4)) {
    return false;
  }
  rgba_srvs_[slot] = resource.srv.Get();
  constants.nv12_mask &= ~(1 << static_cast<int>(slot));
  constants.planar_yuv_mask &= ~(1 << static_cast<int>(slot));
  return true;
}

bool WindowsD3D11ViewportRenderer::prepare_cpu_nv12_track(
    size_t slot,
    const CpuNv12FrameStorage& storage,
    ShaderConstants& constants) {
  const int width = storage.coded_width;
  const int height = storage.coded_height;
  const int bytes = storage.is_p010 ? 2 : 1;
  if (!storage.data || !valid_dimensions(width, height) ||
      (width & 1) != 0 || (height & 1) != 0 ||
      storage.y_stride < width * bytes ||
      storage.uv_stride < width * bytes) {
    return fail("CPU NV12/P010 frame storage is invalid");
  }
  const size_t y_bytes = static_cast<size_t>(storage.y_stride) *
                         static_cast<size_t>(height);
  const size_t required = y_bytes +
      static_cast<size_t>(storage.uv_stride) * static_cast<size_t>(height / 2);
  if (storage.data->size() < required) {
    return fail("CPU NV12/P010 frame buffer is undersized");
  }
  auto& resources = tracks_[slot];
  const DXGI_FORMAT y_format = storage.is_p010 ? DXGI_FORMAT_R16_UNORM
                                               : DXGI_FORMAT_R8_UNORM;
  const DXGI_FORMAT uv_format = storage.is_p010 ? DXGI_FORMAT_R16G16_UNORM
                                                : DXGI_FORMAT_R8G8_UNORM;
  if (!ensure_plane(resources.y, width, height, y_format) ||
      !ensure_plane(resources.uv, width / 2, height / 2, uv_format) ||
      !upload_plane(resources.y,
                    storage.data->data(),
                    storage.y_stride,
                    bytes) ||
      !upload_plane(resources.uv,
                    storage.data->data() + y_bytes,
                    storage.uv_stride,
                    bytes * 2)) {
    return false;
  }
  y_srvs_[slot] = resources.y.srv.Get();
  uv_srvs_[slot] = resources.uv.srv.Get();
  constants.nv12_mask |= (1 << static_cast<int>(slot));
  constants.planar_yuv_mask &= ~(1 << static_cast<int>(slot));
  return true;
}

bool WindowsD3D11ViewportRenderer::prepare_cpu_planar_track(
    size_t slot,
    const CpuPlanarYuvFrameStorage& storage,
    ShaderConstants& constants) {
  if (!storage.planes[0] || !storage.planes[1] ||
      !valid_dimensions(storage.plane_widths[0], storage.plane_heights[0]) ||
      storage.bytes_per_sample < 1 || storage.bytes_per_sample > 2) {
    return fail("CPU planar YUV frame storage is invalid");
  }
  const bool is_16_bit = storage.bytes_per_sample == 2;
  const bool shift = is_16_bit &&
      storage.sample_alignment == CpuYuvSampleAlignment::Packed;
  auto& resources = tracks_[slot];
  const DXGI_FORMAT one_channel = is_16_bit ? DXGI_FORMAT_R16_UNORM
                                            : DXGI_FORMAT_R8_UNORM;
  if (!ensure_plane(resources.y,
                    storage.plane_widths[0],
                    storage.plane_heights[0],
                    one_channel) ||
      !upload_plane(resources.y,
                    storage.planes[0],
                    storage.strides[0],
                    storage.bytes_per_sample,
                    shift)) {
    return false;
  }
  y_srvs_[slot] = resources.y.srv.Get();

  if (storage.plane_layout == CpuYuvPlaneLayout::SemiPlanarYuv420) {
    const DXGI_FORMAT two_channel = is_16_bit ? DXGI_FORMAT_R16G16_UNORM
                                              : DXGI_FORMAT_R8G8_UNORM;
    if (!ensure_plane(resources.uv,
                      storage.plane_widths[1],
                      storage.plane_heights[1],
                      two_channel) ||
        !upload_plane(resources.uv,
                      storage.planes[1],
                      storage.strides[1],
                      storage.bytes_per_sample * 2,
                      shift)) {
      return false;
    }
    uv_srvs_[slot] = resources.uv.srv.Get();
    constants.nv12_mask |= (1 << static_cast<int>(slot));
    constants.planar_yuv_mask &= ~(1 << static_cast<int>(slot));
    return true;
  }

  if (!storage.planes[2] ||
      !ensure_plane(resources.u,
                    storage.plane_widths[1],
                    storage.plane_heights[1],
                    one_channel) ||
      !ensure_plane(resources.v,
                    storage.plane_widths[2],
                    storage.plane_heights[2],
                    one_channel) ||
      !upload_plane(resources.u,
                    storage.planes[1],
                    storage.strides[1],
                    storage.bytes_per_sample,
                    shift) ||
      !upload_plane(resources.v,
                    storage.planes[2],
                    storage.strides[2],
                    storage.bytes_per_sample,
                    shift)) {
    return false;
  }
  u_srvs_[slot] = resources.u.srv.Get();
  v_srvs_[slot] = resources.v.srv.Get();
  constants.nv12_mask &= ~(1 << static_cast<int>(slot));
  constants.planar_yuv_mask |= (1 << static_cast<int>(slot));
  return true;
}

bool WindowsD3D11ViewportRenderer::ensure_plane(PlaneResource& resource,
                                                int width,
                                                int height,
                                                DXGI_FORMAT format) {
  if (!valid_dimensions(width, height)) {
    return fail("viewport plane dimensions are invalid");
  }
  if (resource.texture && resource.width == width &&
      resource.height == height && resource.format == format) {
    return true;
  }
  resource = {};
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  HRESULT result = device_->CreateTexture2D(&desc, nullptr, &resource.texture);
  if (FAILED(result)) {
    return fail("could not create viewport upload texture");
  }
  result = device_->CreateShaderResourceView(resource.texture.Get(),
                                             nullptr,
                                             &resource.srv);
  if (FAILED(result)) {
    resource = {};
    return fail("could not create viewport upload SRV");
  }
  resource.width = width;
  resource.height = height;
  resource.format = format;
  ++stats_.resource_rebuild_count;
  return true;
}

bool WindowsD3D11ViewportRenderer::upload_plane(
    PlaneResource& resource,
    const uint8_t* data,
    int stride,
    int bytes_per_sample,
    bool shift_10_bit_to_msb) {
  if (!resource.texture || !data || stride <= 0 || bytes_per_sample <= 0) {
    return fail("viewport upload plane arguments are invalid");
  }
  const int row_bytes = resource.width * bytes_per_sample;
  if (stride < row_bytes) {
    return fail("viewport upload plane stride is undersized");
  }
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT result = context_->Map(resource.texture.Get(),
                                       0,
                                       D3D11_MAP_WRITE_DISCARD,
                                       0,
                                       &mapped);
  if (FAILED(result)) {
    return fail("could not map viewport upload texture");
  }
  for (int row = 0; row < resource.height; ++row) {
    auto* destination = static_cast<uint8_t*>(mapped.pData) +
        static_cast<size_t>(row) * mapped.RowPitch;
    const auto* source = data + static_cast<size_t>(row) * stride;
    if (!shift_10_bit_to_msb) {
      std::memcpy(destination, source, static_cast<size_t>(row_bytes));
      continue;
    }
    for (int byte = 0; byte < row_bytes; byte += 2) {
      uint16_t sample = 0;
      std::memcpy(&sample, source + byte, sizeof(sample));
      sample = static_cast<uint16_t>((sample & 0x03ffu) << 6u);
      std::memcpy(destination + byte, &sample, sizeof(sample));
    }
  }
  context_->Unmap(resource.texture.Get(), 0);
  return true;
}

bool WindowsD3D11ViewportRenderer::ensure_hardware_copy(
    TrackResources& resources,
    ID3D11Texture2D* source) {
  D3D11_TEXTURE2D_DESC source_desc = {};
  source->GetDesc(&source_desc);
  if (resources.hardware_copy &&
      resources.hardware_width == static_cast<int>(source_desc.Width) &&
      resources.hardware_height == static_cast<int>(source_desc.Height) &&
      resources.hardware_format == source_desc.Format) {
    return true;
  }
  resources.hardware_copy.Reset();
  resources.hardware_y_srv.Reset();
  resources.hardware_uv_srv.Reset();
  resources.cached_hardware_frame_ref.reset();
  resources.cached_hardware_source = nullptr;
  resources.cached_hardware_array_index = -1;
  D3D11_TEXTURE2D_DESC copy_desc = source_desc;
  copy_desc.MipLevels = 1;
  copy_desc.ArraySize = 1;
  copy_desc.Usage = D3D11_USAGE_DEFAULT;
  copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  copy_desc.CPUAccessFlags = 0;
  copy_desc.MiscFlags = 0;
  HRESULT result = device_->CreateTexture2D(&copy_desc,
                                            nullptr,
                                            &resources.hardware_copy);
  if (FAILED(result)) {
    return fail("could not create D3D11VA shader-readable copy texture");
  }
  if (!create_plane_srvs(resources.hardware_copy.Get(),
                         source_desc.Format,
                         resources.hardware_y_srv,
                         resources.hardware_uv_srv)) {
    resources.hardware_copy.Reset();
    return false;
  }
  resources.hardware_width = static_cast<int>(source_desc.Width);
  resources.hardware_height = static_cast<int>(source_desc.Height);
  resources.hardware_format = source_desc.Format;
  ++stats_.resource_rebuild_count;
  return true;
}

bool WindowsD3D11ViewportRenderer::create_plane_srvs(
    ID3D11Texture2D* texture,
    DXGI_FORMAT format,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& y,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& uv) {
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipLevels = 1;
  desc.Format = y_plane_format(format);
  HRESULT result = device_->CreateShaderResourceView(texture, &desc, &y);
  if (FAILED(result)) {
    return fail("could not create D3D11VA Y-plane SRV");
  }
  desc.Format = uv_plane_format(format);
  result = device_->CreateShaderResourceView(texture, &desc, &uv);
  if (FAILED(result)) {
    y.Reset();
    return fail("could not create D3D11VA UV-plane SRV");
  }
  return true;
}

void WindowsD3D11ViewportRenderer::clear_track_bindings(
    size_t slot,
    ShaderConstants& constants) {
  rgba_srvs_[slot] = nullptr;
  y_srvs_[slot] = nullptr;
  uv_srvs_[slot] = nullptr;
  u_srvs_[slot] = nullptr;
  v_srvs_[slot] = nullptr;
  constants.video_aspect[slot] = -1.0f;
  constants.nv12_mask &= ~(1 << static_cast<int>(slot));
  constants.planar_yuv_mask &= ~(1 << static_cast<int>(slot));
}

void WindowsD3D11ViewportRenderer::unbind_shader_resources() {
  if (!context_) {
    return;
  }
  std::array<ID3D11ShaderResourceView*, kMaxTracks> empty{};
  context_->PSSetShaderResources(0, static_cast<UINT>(kMaxTracks), empty.data());
  context_->PSSetShaderResources(4, static_cast<UINT>(kMaxTracks), empty.data());
  context_->PSSetShaderResources(8, static_cast<UINT>(kMaxTracks), empty.data());
  context_->PSSetShaderResources(12, static_cast<UINT>(kMaxTracks), empty.data());
  context_->PSSetShaderResources(16, static_cast<UINT>(kMaxTracks), empty.data());
}

bool WindowsD3D11ViewportRenderer::fail(std::string error) {
  last_error_ = std::move(error);
  return false;
}

}  // namespace vr
