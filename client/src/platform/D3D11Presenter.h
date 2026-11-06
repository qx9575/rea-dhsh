#pragma once

#include "platform/Win32Window.h"
#include "render/Image.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace dhsh {

class D3D11Presenter {
public:
    D3D11Presenter(HWND window,
        std::uint32_t logical_width,
        std::uint32_t logical_height);

    void resize(std::uint32_t width, std::uint32_t height);
    void present(const Image& framebuffer, const Viewport& viewport);

private:
    void create_device_and_swap_chain();
    void create_frame_resources();
    void create_render_target();

    HWND window_;
    std::uint32_t logical_width_;
    std::uint32_t logical_height_;
    std::uint32_t window_width_ = 0;
    std::uint32_t window_height_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture_view_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};

} // namespace dhsh
