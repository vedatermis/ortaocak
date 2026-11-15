// Minimal glTF writer for the PBR tests: accessors in one embedded buffer (data URI), 16- or 32-bit
// indices, materials and images supplied as raw JSON by the test.
#pragma once

#include "tests/assets/AssetTestUtil.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace vo::tests::assets {

inline std::string base64Encode(const std::vector<std::uint8_t>& data)
{
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3)
    {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                data[i + 2];
        out += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], k[v & 63]};
    }
    if (i + 1 == data.size())
    {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out += {k[(v >> 18) & 63], k[(v >> 12) & 63], '=', '='};
    }
    else if (i + 2 == data.size())
    {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], '='};
    }
    return out;
}

class PbrGltfWriter
{
public:
    int floats(const std::vector<float>& v, std::string_view type, bool minmax = false)
    {
        const std::size_t comps = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : 4;
        std::string extra;
        if (minmax)
        {
            std::string mn, mx;
            for (std::size_t c = 0; c < comps; ++c)
            {
                float lo = 1e30f, hi = -1e30f;
                for (std::size_t i = c; i < v.size(); i += comps)
                {
                    lo = std::min(lo, v[i]);
                    hi = std::max(hi, v[i]);
                }
                mn += std::format("{}{}", c ? "," : "", lo);
                mx += std::format("{}{}", c ? "," : "", hi);
            }
            extra = std::format(R"(,"min":[{}],"max":[{}])", mn, mx);
        }
        return add(v.data(), v.size() * 4, 5126, v.size() / comps, type, extra, false);
    }
    int indices32(const std::vector<std::uint32_t>& v) { return add(v.data(), v.size() * 4, 5125, v.size(), "SCALAR", "", true); }
    // Raw bytes as a bufferView (for embedded images); returns the bufferView index.
    int view(const std::vector<std::uint8_t>& bytes)
    {
        align();
        const std::size_t offset = m_bin.size();
        m_bin.insert(m_bin.end(), bytes.begin(), bytes.end());
        const int index = m_viewCount++;
        m_views += std::format(R"({}{{"buffer":0,"byteOffset":{},"byteLength":{}}})", index ? "," : "", offset, bytes.size());
        return index;
    }
    std::string buffersJson() const
    {
        return std::format(R"("buffers":[{{"byteLength":{},"uri":"data:application/octet-stream;base64,{}"}}],"bufferViews":[{}],"accessors":[{}])",
                           m_bin.size(), base64Encode(m_bin), m_views, m_accessors);
    }

private:
    void align()
    {
        while (m_bin.size() % 4)
            m_bin.push_back(0);
    }
    int add(const void* data, std::size_t size, int componentType, std::size_t count, std::string_view type,
            const std::string& extra, bool isIndex)
    {
        align();
        const std::size_t offset = m_bin.size();
        const auto* p = static_cast<const std::uint8_t*>(data);
        m_bin.insert(m_bin.end(), p, p + size);
        const int viewIndex = m_viewCount++;
        m_views += std::format(R"({}{{"buffer":0,"byteOffset":{},"byteLength":{}{}}})", viewIndex ? "," : "", offset,
                               size, isIndex ? R"(,"target":34963)" : "");
        const int index = m_accessorCount++;
        m_accessors += std::format(R"({}{{"bufferView":{},"componentType":{},"count":{},"type":"{}"{}}})",
                                   index ? "," : "", viewIndex, componentType, count, type, extra);
        return index;
    }

    std::vector<std::uint8_t> m_bin;
    std::string m_views, m_accessors;
    int m_viewCount = 0;
    int m_accessorCount = 0;
};

} // namespace vo::tests::assets
