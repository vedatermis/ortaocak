// PNG reading/writing for assetc (stb_image / stb_image_write, implemented in StbImpl.cpp, which
// lives only in the assetc library because the client defines its own stb_image implementation).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vo::tools::assetc {

struct Image8
{
    int width = 0;
    int height = 0;
    int channels = 0; // channels stored in `pixels` (the requested count)
    std::vector<std::uint8_t> pixels;
};

struct Image16
{
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> pixels; // one channel
};

// Loads a PNG converted to `channels` channels (1..4).
[[nodiscard]] bool loadImage8(const std::filesystem::path& path, int channels, Image8& out, std::string& error);
// Loads a grayscale PNG as 16-bit (8-bit sources are expanded by ×257).
[[nodiscard]] bool loadImage16(const std::filesystem::path& path, Image16& out, std::string& error);

// Writes an 8-bit PNG (used by tests to create terrain inputs).
[[nodiscard]] bool writePng8(const std::filesystem::path& path, const Image8& image, std::string& error);

} // namespace vo::tools::assetc
