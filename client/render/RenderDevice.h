// D3D11 device + flip-model swap chain for one window. The back buffer is B8G8R8A8_UNORM; it is
// rendered through a B8G8R8A8_UNORM_SRGB view so shading and blending happen in linear space.
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

    // Binds the back buffer + depth buffer and viewport, clears colour and depth (to 0: reverse-Z
    // far plane, docs/CLIENT_RENDERING.md).
    void beginFrame(const float clearColor[4]);
    // Re-binds the back buffer, depth buffer and full viewport (after an offscreen pass).
    void bindBackBuffer();
    // Copies the current back buffer to CPU memory. Call after rendering, before present().
    bool captureBackBuffer(CapturedImage& out, std::string& error);
    bool present(std::string& error);

    [[nodiscard]] ID3D11Device* device() const noexcept { return m_device.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return m_context.Get(); }
    [[nodiscard]] const std::string& adapterName() const noexcept { return m_adapterName; }
    [[nodiscard]] bool isWarp() const noexcept { return m_isWarp; }
    [[nodiscard]] bool debugLayerActive() const noexcept { return m_debugLayer; }
    [[nodiscard]] D3D_FEATURE_LEVEL featureLevel() const noexcept { return m_featureLevel; }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    // D32_FLOAT depth buffer matching the back buffer.
    [[nodiscard]] ID3D11DepthStencilView* depthView() const noexcept { return m_depthDsv.Get(); }
    [[nodiscard]] ID3D11RenderTargetView* backBufferView() const noexcept { return m_backBufferRtv.Get(); }

    // Number of live-object reports produced by the last shutdown() (debug layer only; 0 = clean).
    [[nodiscard]] std::size_t liveObjectMessages() const noexcept { return m_liveObjectMessages; }

private:
    bool createDevice(bool warp, bool debug, std::string& error);
    bool createSwapChain(HWND hwnd, std::string& error);
    bool createBackBufferView(std::string& error);
    bool createDepthBuffer(std::string& error);
    void logDebugMessages() noexcept;
    void reportLiveObjects() noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backBufferRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depth;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthDsv;

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
