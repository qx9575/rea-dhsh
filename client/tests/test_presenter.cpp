#include "platform/D3D11Presenter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

dhsh::Image test_frame() {
    dhsh::Image frame;
    frame.width = 800;
    frame.height = 600;
    frame.rgba.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
    for (std::size_t offset = 0; offset < frame.rgba.size(); offset += 4) {
        frame.rgba[offset] = std::byte{0x80};
        frame.rgba[offset + 1] = std::byte{0x40};
        frame.rgba[offset + 2] = std::byte{0x20};
        frame.rgba[offset + 3] = std::byte{0xff};
    }
    return frame;
}

void test_resize_and_minimize_after_first_present() {
    dhsh::Win32Window window(L"dhsh_presenter_resize_test", 800, 600,
        [](const dhsh::WindowEvent&) {});
    dhsh::D3D11Presenter presenter(window.handle(), 800, 600);
    dhsh::Viewport viewport(800, 600);
    const dhsh::Image frame = test_frame();

    presenter.present(frame, viewport);
    for (const auto [width, height] : {std::pair<UINT, UINT>{1000, 700},
             {640, 480}, {1200, 800}, {800, 600}}) {
        presenter.resize(width, height);
        viewport.resize(width, height);
        presenter.present(frame, viewport);
    }

    presenter.resize(0, 0);
    viewport.resize(0, 0);
    presenter.present(frame, viewport);
    presenter.resize(1000, 800);
    viewport.resize(1000, 800);
    presenter.present(frame, viewport);
}

void test_resize_seam_observes_no_bound_backbuffer() {
    dhsh::Win32Window window(L"dhsh_presenter_unbind_test", 800, 600,
        [](const dhsh::WindowEvent&) {});
    dhsh::D3D11PresenterHooks hooks;
    std::size_t resize_calls = 0;
    hooks.resize_buffers = [&](ID3D11DeviceContext& context,
        IDXGISwapChain& swap_chain, UINT width, UINT height) {
        ++resize_calls;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bound;
        context.OMGetRenderTargets(1, &bound, nullptr);
        if (bound != nullptr) {
            return DXGI_ERROR_INVALID_CALL;
        }
        return swap_chain.ResizeBuffers(
            0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    };
    dhsh::D3D11Presenter presenter(window.handle(), 800, 600, hooks);
    dhsh::Viewport viewport(800, 600);
    const dhsh::Image frame = test_frame();
    presenter.present(frame, viewport);
    presenter.resize(1000, 700);
    check(resize_calls == 1, "resize seam was not exercised");
}

void expect_device_lost_recovery(HRESULT injected_status) {
    dhsh::Win32Window window(L"dhsh_presenter_recovery_test", 800, 600,
        [](const dhsh::WindowEvent&) {});
    dhsh::D3D11PresenterHooks hooks;
    std::size_t present_calls = 0;
    hooks.present = [&](IDXGISwapChain& swap_chain, UINT sync_interval, UINT flags) {
        ++present_calls;
        if (present_calls == 2) {
            return injected_status;
        }
        return swap_chain.Present(sync_interval, flags);
    };
    dhsh::D3D11Presenter presenter(window.handle(), 800, 600, hooks);
    dhsh::Viewport viewport(800, 600);
    const dhsh::Image frame = test_frame();

    presenter.present(frame, viewport);
    presenter.present(frame, viewport);
    check(present_calls == 3, "device-lost frame was not retried exactly once");
    check(presenter.device_recovery_count() == 1,
        "device-lost status did not rebuild the device exactly once");
    presenter.present(frame, viewport);
    check(present_calls == 4 && presenter.device_recovery_count() == 1,
        "recovered presenter did not remain usable");
}

void test_device_lost_statuses_recover_and_retry() {
    expect_device_lost_recovery(DXGI_ERROR_DEVICE_REMOVED);
    expect_device_lost_recovery(DXGI_ERROR_DEVICE_RESET);
    expect_device_lost_recovery(DXGI_ERROR_DEVICE_HUNG);
}

void test_resize_device_removed_recreates_swapchain() {
    dhsh::Win32Window window(L"dhsh_presenter_resize_recovery_test", 800, 600,
        [](const dhsh::WindowEvent&) {});
    dhsh::D3D11PresenterHooks hooks;
    std::size_t resize_calls = 0;
    hooks.resize_buffers = [&](ID3D11DeviceContext&, IDXGISwapChain& swap_chain,
        UINT width, UINT height) {
        ++resize_calls;
        if (resize_calls == 1) {
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        return swap_chain.ResizeBuffers(
            0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    };
    dhsh::D3D11Presenter presenter(window.handle(), 800, 600, hooks);
    dhsh::Viewport viewport(800, 600);
    const dhsh::Image frame = test_frame();
    presenter.present(frame, viewport);
    presenter.resize(1000, 700);
    check(resize_calls == 1 && presenter.device_recovery_count() == 1,
        "resize device removal did not recreate the device and swapchain");
    viewport.resize(1000, 700);
    presenter.present(frame, viewport);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"resize and minimize after first present", test_resize_and_minimize_after_first_present},
        {"resize unbinds the backbuffer", test_resize_seam_observes_no_bound_backbuffer},
        {"device-lost recovery", test_device_lost_statuses_recover_and_retry},
        {"resize device-lost recovery", test_resize_device_removed_recreates_swapchain},
    };

    bool passed = true;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
