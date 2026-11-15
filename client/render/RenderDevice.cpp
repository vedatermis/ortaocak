#include "client/render/RenderDevice.h"

#include "client/platform/Win32Util.h"
#include "shared/core/Log.h"

#include <dxgi1_3.h>
#include <dxgidebug.h>

#include <array>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace vo::client::render {
namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kBackBufferViewFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
constexpr UINT kBackBufferCount = 2;

std::string hr(HRESULT value)
{
    return vo::client::platform::hresultToString(value);
}

const char* featureLevelName(D3D_FEATURE_LEVEL level)
{
    switch (level)
    {
    case D3D_FEATURE_LEVEL_11_1:
        return "11_1";
    case D3D_FEATURE_LEVEL_11_0:
        return "11_0";
    default:
        return "?";
    }
}

ComPtr<IDXGIInfoQueue> dxgiInfoQueue()
{
    ComPtr<IDXGIInfoQueue> queue;
    if (FAILED(::DXGIGetDebugInterface1(0, IID_PPV_ARGS(&queue))))
        return nullptr;
    return queue;
}

// Logs and removes every stored DXGI/D3D debug message; returns how many there were.
std::size_t drainInfoQueue(IDXGIInfoQueue* queue, vo::log::Level level, const char* prefix)
{
    if (queue == nullptr)
        return 0;
    const UINT64 count = queue->GetNumStoredMessages(DXGI_DEBUG_ALL);
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T length = 0;
        if (FAILED(queue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &length)) || length == 0)
            continue;
        std::vector<std::byte> buffer(length);
        auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buffer.data());
        if (FAILED(queue->GetMessage(DXGI_DEBUG_ALL, i, message, &length)))
            continue;
        vo::log::writeMessage(level, std::format("{}{}", prefix, message->pDescription), nullptr, 0);
    }
    queue->ClearStoredMessages(DXGI_DEBUG_ALL);
    return static_cast<std::size_t>(count);
}

} // namespace

RenderDevice::~RenderDevice()
{
    shutdown();
}

bool RenderDevice::init(const Desc& desc, std::string& error)
{
    m_vsync = desc.vsync;
    m_width = desc.width > 0 ? desc.width : 1;
    m_height = desc.height > 0 ? desc.height : 1;

    bool created = false;
    std::string lastError;
    const std::array<bool, 2> attempts{false, true}; // hardware, then WARP
    for (const bool warp : attempts)
    {
        if (desc.forceWarp && !warp)
            continue;
        if (desc.debugLayer && createDevice(warp, true, lastError))
        {
            created = true;
            break;
        }
        if (desc.debugLayer)
            VO_LOG_WARN("D3D11 debug layer unavailable ({}); continuing without it", lastError);
        if (createDevice(warp, false, lastError))
        {
            created = true;
            break;
        }
        VO_LOG_WARN("{} device creation failed: {}", warp ? "WARP" : "hardware", lastError);
    }
    if (!created)
    {
        error = "D3D11CreateDevice failed: " + lastError;
        return false;
    }

    VO_LOG_INFO("D3D11 device: adapter '{}'{}, feature level {}, debug layer {}", m_adapterName,
                m_isWarp ? " (WARP)" : "", featureLevelName(m_featureLevel), m_debugLayer ? "on" : "off");

    if (!createSwapChain(desc.hwnd, error))
        return false;
    if (!m_post.init(m_device.Get(), error))
        return false;
    return createBackBufferView(error);
}

bool RenderDevice::createDevice(bool warp, bool debug, std::string& error)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (debug)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    const std::array<D3D_FEATURE_LEVEL, 2> levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const HRESULT res = ::D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                            nullptr, flags, levels.data(), static_cast<UINT>(levels.size()),
                                            D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(res))
    {
        error = hr(res);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&adapterDesc)))
    {
        m_adapterName = vo::client::platform::toUtf8(adapterDesc.Description);
    }
    else
    {
        m_adapterName = "<unknown>";
    }

    m_device = std::move(device);
    m_context = std::move(context);
    m_featureLevel = level;
    m_isWarp = warp;
    m_debugLayer = debug;
    return true;
}

bool RenderDevice::createSwapChain(HWND hwnd, std::string& error)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT res = m_device.As(&dxgiDevice);
    if (SUCCEEDED(res))
        res = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(res))
        res = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(res))
    {
        error = "cannot get IDXGIFactory2: " + hr(res);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = m_width;
    sd.Height = m_height;
    sd.Format = kBackBufferFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBackBufferCount;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    res = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &sd, nullptr, nullptr, &m_swapChain);
    if (FAILED(res))
    {
        error = "CreateSwapChainForHwnd failed: " + hr(res);
        return false;
    }
    // Alt+Enter fullscreen is handled by the game later (borderless); disable DXGI's version.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    VO_LOG_INFO("swap chain: {}x{}, flip-discard, {} buffers, vsync {}", m_width, m_height, kBackBufferCount,
                m_vsync ? "on" : "off");
    return true;
}

bool RenderDevice::createBackBufferView(std::string& error)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT res = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(res))
    {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = kBackBufferViewFormat;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        res = m_device->CreateRenderTargetView(backBuffer.Get(), &rtvDesc, &m_backBufferRtv);
    }
    if (FAILED(res))
    {
        error = "back buffer RTV creation failed: " + hr(res);
        return false;
    }
    return createDepthBuffer(error);
}

bool RenderDevice::createDepthBuffer(std::string& error)
{
    m_depthDsv.Reset();
    m_depth.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    HRESULT res = m_device->CreateTexture2D(&desc, nullptr, &m_depth);
    if (SUCCEEDED(res))
        res = m_device->CreateDepthStencilView(m_depth.Get(), nullptr, &m_depthDsv);
    if (FAILED(res))
    {
        error = "depth buffer creation failed: " + hr(res);
        return false;
    }
    const char* name = "backbuffer.depth";
    m_depth->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
    return true;
}

bool RenderDevice::resize(std::uint32_t width, std::uint32_t height, std::string& error)
{
    if (!m_swapChain || width == 0 || height == 0)
        return true;
    if (width == m_width && height == m_height)
        return true;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_backBufferRtv.Reset();
    m_depthDsv.Reset();
    m_depth.Reset();
    m_context->Flush();

    const HRESULT res = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(res))
    {
        error = "ResizeBuffers failed: " + hr(res);
        return false;
    }
    m_width = width;
    m_height = height;
    VO_LOG_DEBUG("swap chain resized to {}x{}", width, height);
    if (!createBackBufferView(error))
        return false;
    return !m_msaaColor || createMsaaTargets(error);
}

void RenderDevice::bindBackBuffer()
{
    ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
    m_context->OMSetRenderTargets(1, &rtv, m_depthDsv.Get());
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void RenderDevice::beginFrame(const float clearColor[4])
{
    bindBackBuffer();
    ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
    if (m_depthDsv)
        m_context->ClearDepthStencilView(m_depthDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0); // reverse-Z far
    // The RTV is sRGB: clear values are linear.
    auto toLinear = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const float linear[4] = {toLinear(clearColor[0]), toLinear(clearColor[1]), toLinear(clearColor[2]), clearColor[3]};
    m_context->ClearRenderTargetView(rtv, linear);
}

bool RenderDevice::captureBackBuffer(CapturedImage& out, std::string& error)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT res = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(res))
    {
        error = "GetBuffer failed: " + hr(res);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    res = m_device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(res))
    {
        error = "staging texture creation failed: " + hr(res);
        return false;
    }
    m_context->CopyResource(staging.Get(), backBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    res = m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(res))
    {
        error = "Map failed: " + hr(res);
        return false;
    }

    out.width = desc.Width;
    out.height = desc.Height;
    out.rowPitch = desc.Width * 4;
    out.bgra.resize(static_cast<std::size_t>(out.rowPitch) * out.height);
    const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t y = 0; y < out.height; ++y)
    {
        std::uint8_t* dst = out.bgra.data() + static_cast<std::size_t>(y) * out.rowPitch;
        std::memcpy(dst, src + static_cast<std::size_t>(y) * mapped.RowPitch, out.rowPitch);
        for (std::uint32_t x = 0; x < out.width; ++x)
            dst[x * 4 + 3] = 255; // swap chain alpha is ignored; make the image opaque
    }
    m_context->Unmap(staging.Get(), 0);
    return true;
}

RenderDevice::PresentResult RenderDevice::present(std::string& error)
{
    const HRESULT res = m_swapChain->Present(m_vsync ? 1 : 0, 0);
    if (m_simulateLoss)
    {
        m_simulateLoss = false;
        error = "device lost (simulated)";
        return PresentResult::DeviceLost;
    }
    if (res == DXGI_ERROR_DEVICE_REMOVED || res == DXGI_ERROR_DEVICE_RESET)
    {
        // The caller recreates the device and every GPU resource (client/main.cpp).
        error = "device lost: " + hr(res) + ", reason " + hr(m_device->GetDeviceRemovedReason());
        return PresentResult::DeviceLost;
    }
    if (FAILED(res))
    {
        error = "Present failed: " + hr(res);
        return PresentResult::Failed;
    }
    return PresentResult::Ok; // includes DXGI_STATUS_OCCLUDED
}

// ---- HDR scene target (optionally multisampled) ----

bool RenderDevice::setMsaa(std::uint32_t samples, std::string& error)
{
    releaseMsaaTargets();
    m_msaaSamples = 1;
    if (!m_device)
        return true;
    // Highest supported count <= the requested one.
    for (std::uint32_t s = std::min<std::uint32_t>(std::max(samples, 1u), D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT); s > 1;
         s /= 2)
    {
        UINT quality = 0;
        if (SUCCEEDED(m_device->CheckMultisampleQualityLevels(kHdrFormat, s, &quality)) && quality > 0)
        {
            UINT depthQuality = 0;
            if (SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, s, &depthQuality)) &&
                depthQuality > 0)
            {
                m_msaaSamples = s;
                break;
            }
        }
    }
    if (m_msaaSamples != std::max(samples, 1u))
        VO_LOG_WARN("graphics.msaa {}: not supported, using {}x", samples, m_msaaSamples);
    if (!createMsaaTargets(error))
    {
        releaseMsaaTargets();
        m_msaaSamples = 1;
        return false;
    }
    VO_LOG_INFO("world pass: HDR R16G16B16A16_FLOAT, {}x MSAA ({}x{}), tone-mapped ({}) and resolved into the sRGB "
                "back buffer",
                m_msaaSamples, m_width, m_height, m_post.toneMap == ToneMap::Clamp ? "clamp" : "ACES fitted");
    return true;
}

bool RenderDevice::createMsaaTargets(std::string& error)
{
    releaseMsaaTargets();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kHdrFormat; // linear radiance x exposure; tone-mapped in resolveScene()
    desc.SampleDesc.Count = m_msaaSamples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT res = m_device->CreateTexture2D(&desc, nullptr, &m_msaaColor);
    if (SUCCEEDED(res))
        res = m_device->CreateRenderTargetView(m_msaaColor.Get(), nullptr, &m_msaaRtv);
    if (SUCCEEDED(res))
        res = m_device->CreateShaderResourceView(m_msaaColor.Get(), nullptr, &m_sceneSrv);
    if (SUCCEEDED(res))
    {
        D3D11_TEXTURE2D_DESC depth = desc;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        res = m_device->CreateTexture2D(&depth, nullptr, &m_msaaDepth);
    }
    if (SUCCEEDED(res))
        res = m_device->CreateDepthStencilView(m_msaaDepth.Get(), nullptr, &m_msaaDsv);
    if (FAILED(res))
    {
        error = "HDR scene target creation failed: " + hr(res);
        return false;
    }
    const char* name = "scene.hdr";
    m_msaaColor->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
    const char* depthName = "scene.depth";
    m_msaaDepth->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(depthName)), depthName);
    return true;
}

void RenderDevice::releaseMsaaTargets() noexcept
{
    m_sceneSrv.Reset();
    m_msaaRtv.Reset();
    m_msaaColor.Reset();
    m_msaaDsv.Reset();
    m_msaaDepth.Reset();
}

std::uint64_t RenderDevice::sceneTargetBytes() const noexcept
{
    // colour 8 B + depth 4 B per sample
    return static_cast<std::uint64_t>(m_width) * m_height * m_msaaSamples * 12ull;
}

ID3D11RenderTargetView* RenderDevice::sceneColorView() const noexcept
{
    return m_msaaRtv ? m_msaaRtv.Get() : m_backBufferRtv.Get();
}

ID3D11DepthStencilView* RenderDevice::sceneDepthView() const noexcept
{
    return m_msaaDsv ? m_msaaDsv.Get() : m_depthDsv.Get();
}

void RenderDevice::beginScene(const float clearColor[4])
{
    if (!m_msaaRtv)
        return;
    auto toLinear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    const float linear[4] = {toLinear(clearColor[0]), toLinear(clearColor[1]), toLinear(clearColor[2]), clearColor[3]};
    m_context->ClearRenderTargetView(m_msaaRtv.Get(), linear);
    m_context->ClearDepthStencilView(m_msaaDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
}

void RenderDevice::resolveScene()
{
    if (!m_msaaRtv)
        return;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_post.resolve(m_context.Get(), m_sceneSrv.Get(), m_msaaSamples, m_backBufferRtv.Get(), m_width, m_height);
    bindBackBuffer();
}

void RenderDevice::logDebugMessages() noexcept
{
    if (!m_debugLayer)
        return;
    auto queue = dxgiInfoQueue();
    const std::size_t n = drainInfoQueue(queue.Get(), vo::log::Level::Warn, "d3d debug: ");
    if (n > 0)
        VO_LOG_WARN("D3D11 debug layer produced {} message(s) during the run", n);
}

void RenderDevice::reportLiveObjects() noexcept
{
    ComPtr<IDXGIDebug1> debug;
    if (FAILED(::DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug))))
        return;
    auto queue = dxgiInfoQueue();
    if (queue)
        queue->ClearStoredMessages(DXGI_DEBUG_ALL);
    debug->ReportLiveObjects(DXGI_DEBUG_ALL,
                             static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
    m_liveObjectMessages = drainInfoQueue(queue.Get(), vo::log::Level::Warn, "live object: ");
    if (m_liveObjectMessages == 0)
        VO_LOG_INFO("D3D11 shutdown: no live objects reported");
    else
        VO_LOG_WARN("D3D11 shutdown: {} live-object report line(s)", m_liveObjectMessages);
}

void RenderDevice::shutdown() noexcept
{
    if (!m_device)
        return;
    logDebugMessages();
    if (m_context)
    {
        m_context->ClearState();
        m_context->Flush();
    }
    releaseMsaaTargets();
    m_post.reset();
    m_backBufferRtv.Reset();
    m_depthDsv.Reset();
    m_depth.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
    m_msaaSamples = 1;
    m_simulateLoss = false;
    if (m_debugLayer)
        reportLiveObjects();
}

} // namespace vo::client::render
