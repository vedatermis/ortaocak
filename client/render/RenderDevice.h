// D3D11 device + flip-model swap chain for one window.
#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vo::client::render {

struct CapturedImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowPitch = 0;      // bytes per row
    std::vector<std::uint8_t> bgra;  // 32bpp BGRA, alpha forced to 255
};

class RenderDevice
{
public:
    struct Desc
    {
        HWND hwnd = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool vsync = true;
        bool debugLayer = false;   // used only if the D3D11 SDK layers are installed
        bool forceWarp = false;    // skip hardware, use the WARP software rasterizer
    };

    RenderDevice() = default;
    ~RenderDevice();
    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;

    bool init(const Desc& desc, std::string& error);
    // Releases every D3D/DXGI object; in debug-layer mode reports live objects afterwards.
    void shutdown() noexcept;

    bool resize(std::uint32_t width, std::uint32_t height, std::string& error);

    // Binds the back buffer and viewport, and clears it.
    void beginFrame(const float clearColor[4]);
    // Copies the current back buffer to CPU memory. Call after rendering, before present().
    bool captureBackBuffer(CapturedImage& out, std::string& error);
    bool present(std::string& error);

    [[nodiscard]] const std::string& adapterName() const noexcept { return m_adapterName; }
    [[nodiscard]] bool isWarp() const noexcept { return m_isWarp; }
    [[nodiscard]] bool debugLayerActive() const noexcept { return m_debugLayer; }
    [[nodiscard]] D3D_FEATURE_LEVEL featureLevel() const noexcept { return m_featureLevel; }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }

    // Number of live-object reports produced by the last shutdown() (debug layer only; 0 = clean).
    [[nodiscard]] std::size_t liveObjectMessages() const noexcept { return m_liveObjectMessages; }

private:
    bool createDevice(bool warp, bool debug, std::string& error);
    bool createSwapChain(HWND hwnd, std::string& error);
    bool createBackBufferView(std::string& error);
    void logDebugMessages() noexcept;
    void reportLiveObjects() noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backBufferRtv;

    std::string m_adapterName;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    bool m_vsync = true;
    bool m_isWarp = false;
    bool m_debugLayer = false;
    std::size_t m_liveObjectMessages = 0;
};

} // namespace vo::client::render
