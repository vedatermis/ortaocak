// Shader/pipeline helper: a vertex shader + input layout + pixel shader created from bytecode that
// fxc compiled at build time (generated headers under <build>/generated/client/shaders/).
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <span>
#include <string>

namespace vo::client::render {

struct ShaderBytecode
{
    const void* data = nullptr;
    std::size_t size = 0;
};

// Wraps a generated `const BYTE name[]` array.
template <std::size_t N>
[[nodiscard]] constexpr ShaderBytecode bytecodeOf(const unsigned char (&array)[N]) noexcept
{
    return {array, N};
}

class Pipeline
{
public:
    Pipeline() = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // `ps` may be empty ({nullptr, 0}) for depth-only passes.
    bool create(ID3D11Device* device, ShaderBytecode vs, ShaderBytecode ps,
                std::span<const D3D11_INPUT_ELEMENT_DESC> layout, const char* debugName, std::string& error);
    void reset() noexcept;

    // Binds input layout, topology and both shaders.
    void bind(ID3D11DeviceContext* context,
              D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) const;

    [[nodiscard]] bool valid() const noexcept { return m_vs != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_layout;
};

// Attaches a debug name visible in graphics debuggers and live-object reports.
void setDebugName(ID3D11DeviceChild* object, const char* name) noexcept;

} // namespace vo::client::render
