#include "platform/D3D11Presenter.h"

#include <d3dcompiler.h>

#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace dhsh {
namespace {

void check_hresult(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) {
        return;
    }
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex << std::uppercase
            << std::setw(8) << std::setfill('0')
            << static_cast<unsigned long>(result);
    throw std::runtime_error(message.str());
}

bool is_device_lost(HRESULT result) {
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
        || result == DXGI_ERROR_DEVICE_HUNG;
}

constexpr char shader_source[] = R"(
Texture2D frame_texture : register(t0);
SamplerState frame_sampler : register(s0);
struct VertexOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(output.uv.x * 2.0 - 1.0,
        1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}
float4 pixel_main(VertexOutput input) : SV_TARGET {
    return frame_texture.Sample(frame_sampler, input.uv);
}
)";

Microsoft::WRL::ComPtr<ID3DBlob> compile_shader(
    const char* entry, const char* profile) {
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(shader_source, sizeof(shader_source) - 1,
        "dhsh_frame_presenter", nullptr, nullptr, entry, profile,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
    if (FAILED(result)) {
        std::string message = "cannot compile D3D11 presentation shader";
        if (errors != nullptr) {
            message += ": ";
            message += static_cast<const char*>(errors->GetBufferPointer());
        }
        throw std::runtime_error(message);
    }
    return shader;
}

} // namespace

D3D11Presenter::D3D11Presenter(HWND window,
    std::uint32_t logical_width,
    std::uint32_t logical_height,
    D3D11PresenterHooks hooks)
    : window_(window), logical_width_(logical_width),
      logical_height_(logical_height), hooks_(std::move(hooks)) {
    RECT client{};
    GetClientRect(window_, &client);
    window_width_ = static_cast<std::uint32_t>(client.right - client.left);
    window_height_ = static_cast<std::uint32_t>(client.bottom - client.top);
    create_device_and_swap_chain();
    create_frame_resources();
    create_render_target();
}

void D3D11Presenter::create_device_and_swap_chain() {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = window_width_;
    description.BufferDesc.Height = window_height_;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = window_;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    check_hresult(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, &feature_level, 1, D3D11_SDK_VERSION, &description,
        &swap_chain_, &device_, nullptr, &context_),
        "D3D11CreateDeviceAndSwapChain");
}

void D3D11Presenter::create_frame_resources() {
    D3D11_TEXTURE2D_DESC texture_description{};
    texture_description.Width = logical_width_;
    texture_description.Height = logical_height_;
    texture_description.MipLevels = 1;
    texture_description.ArraySize = 1;
    texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_description.SampleDesc.Count = 1;
    texture_description.Usage = D3D11_USAGE_DYNAMIC;
    texture_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hresult(device_->CreateTexture2D(&texture_description, nullptr, &texture_),
        "CreateTexture2D(framebuffer)");
    check_hresult(device_->CreateShaderResourceView(
        texture_.Get(), nullptr, &texture_view_),
        "CreateShaderResourceView(framebuffer)");

    const auto vertex_bytes = compile_shader("vertex_main", "vs_4_0");
    check_hresult(device_->CreateVertexShader(vertex_bytes->GetBufferPointer(),
        vertex_bytes->GetBufferSize(), nullptr, &vertex_shader_),
        "CreateVertexShader(framebuffer)");
    const auto pixel_bytes = compile_shader("pixel_main", "ps_4_0");
    check_hresult(device_->CreatePixelShader(pixel_bytes->GetBufferPointer(),
        pixel_bytes->GetBufferSize(), nullptr, &pixel_shader_),
        "CreatePixelShader(framebuffer)");

    D3D11_SAMPLER_DESC sampler_description{};
    sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
    check_hresult(device_->CreateSamplerState(&sampler_description, &sampler_),
        "CreateSamplerState(framebuffer)");
}

void D3D11Presenter::create_render_target() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    check_hresult(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)),
        "IDXGISwapChain::GetBuffer");
    check_hresult(device_->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &render_target_),
        "CreateRenderTargetView(back buffer)");
}

void D3D11Presenter::release_render_target() {
    if (context_ != nullptr) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->ClearState();
        context_->Flush();
    }
    render_target_.Reset();
}

void D3D11Presenter::recover_device() {
    release_render_target();
    texture_view_.Reset();
    texture_.Reset();
    vertex_shader_.Reset();
    pixel_shader_.Reset();
    sampler_.Reset();
    swap_chain_.Reset();
    context_.Reset();
    device_.Reset();
    create_device_and_swap_chain();
    create_frame_resources();
    if (window_width_ != 0 && window_height_ != 0) {
        create_render_target();
    }
    ++device_recovery_count_;
}

void D3D11Presenter::resize(std::uint32_t width, std::uint32_t height) {
    if (width == window_width_ && height == window_height_) {
        return;
    }
    window_width_ = width;
    window_height_ = height;
    release_render_target();
    if (width == 0 || height == 0) {
        return;
    }
    const HRESULT result = hooks_.resize_buffers
        ? hooks_.resize_buffers(*context_.Get(), *swap_chain_.Get(), width, height)
        : swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (is_device_lost(result)) {
        recover_device();
        return;
    }
    check_hresult(result, "IDXGISwapChain::ResizeBuffers");
    create_render_target();
}

void D3D11Presenter::present(const Image& framebuffer, const Viewport& viewport) {
    if (window_width_ == 0 || window_height_ == 0) {
        return;
    }
    if (framebuffer.width != logical_width_ || framebuffer.height != logical_height_
        || framebuffer.rgba.size()
            != static_cast<std::size_t>(logical_width_) * logical_height_ * 4) {
        throw std::invalid_argument("presenter requires the logical RGBA framebuffer");
    }

    HRESULT result = present_once(framebuffer, viewport);
    if (is_device_lost(result)) {
        recover_device();
        result = present_once(framebuffer, viewport);
    }
    check_hresult(result, "D3D11 framebuffer presentation");
}

HRESULT D3D11Presenter::present_once(
    const Image& framebuffer, const Viewport& viewport) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_result =
        context_->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(map_result)) {
        return map_result;
    }
    const auto* source = reinterpret_cast<const unsigned char*>(framebuffer.rgba.data());
    auto* destination = static_cast<unsigned char*>(mapped.pData);
    const std::size_t source_pitch = static_cast<std::size_t>(logical_width_) * 4;
    for (std::uint32_t row = 0; row < logical_height_; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * mapped.RowPitch,
            source + static_cast<std::size_t>(row) * source_pitch, source_pitch);
    }
    context_->Unmap(texture_.Get(), 0);

    const float black[] = {0.0F, 0.0F, 0.0F, 1.0F};
    context_->ClearRenderTargetView(render_target_.Get(), black);
    ID3D11RenderTargetView* target = render_target_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT d3d_viewport{};
    d3d_viewport.TopLeftX = static_cast<float>(viewport.x());
    d3d_viewport.TopLeftY = static_cast<float>(viewport.y());
    d3d_viewport.Width = static_cast<float>(viewport.width());
    d3d_viewport.Height = static_cast<float>(viewport.height());
    d3d_viewport.MinDepth = 0.0F;
    d3d_viewport.MaxDepth = 1.0F;
    context_->RSSetViewports(1, &d3d_viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* texture_view = texture_view_.Get();
    context_->PSSetShaderResources(0, 1, &texture_view);
    ID3D11SamplerState* sampler = sampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    context_->Draw(3, 0);
    return hooks_.present ? hooks_.present(*swap_chain_.Get(), 1, 0)
                          : swap_chain_->Present(1, 0);
}

} // namespace dhsh
