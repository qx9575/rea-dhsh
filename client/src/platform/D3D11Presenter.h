#pragma once

#include "platform/Win32Window.h"
#include "render/Image.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>

namespace dhsh {

struct D3D11PresenterHooks {
    std::function<HRESULT(IDXGISwapChain&, UINT, UINT)> present;
    std::function<HRESULT(ID3D11DeviceContext&, IDXGISwapChain&, UINT, UINT)>
        resize_buffers;
};

class D3D11Presenter {
public:
    D3D11Presenter(HWND window,
        std::uint32_t logical_width,
        std::uint32_t logical_height,
        D3D11PresenterHooks hooks = {});

    void resize(std::uint32_t width, std::uint32_t height);
    void present(const Image& framebuffer, const Viewport& viewport);
    std::uint32_t device_recovery_count() const noexcept {
        return device_recovery_count_;
    }

private:
    void create_device_and_swap_chain();
    void create_frame_resources();
    void create_render_target();
    void release_render_target();
    void recover_device();
    HRESULT present_once(const Image& framebuffer, const Viewport& viewport);

    HWND window_;
    std::uint32_t logical_width_;
    std::uint32_t logical_height_;
    std::uint32_t window_width_ = 0;
    std::uint32_t window_height_ = 0;
    D3D11PresenterHooks hooks_;
    std::uint32_t device_recovery_count_ = 0;
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
