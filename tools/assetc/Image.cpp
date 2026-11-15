#include "tools/assetc/Image.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Paths.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <cstring>
#include <format>
#include <limits>

namespace vo::tools::assetc {

namespace {

bool readAll(const std::filesystem::path& path, std::vector<std::byte>& bytes, std::string& error)
{
    if (!vo::assets::readFileBytes(path, bytes, error))
        return false;
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        error = std::format("'{}' is too large", core::pathToUtf8(path));
        return false;
    }
    return true;
}

void appendToVector(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<std::byte>*>(context);
    const auto* p = static_cast<const std::byte*>(data);
    out->insert(out->end(), p, p + size);
}

} // namespace

bool loadImage8(const std::filesystem::path& path, int channels, Image8& out, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!readAll(path, bytes, error))
        return false;
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                            static_cast<int>(bytes.size()), &w, &h, &n, channels);
    if (pixels == nullptr)
    {
        error = std::format("'{}': cannot decode PNG ({})", core::pathToUtf8(path), stbi_failure_reason());
        return false;
    }
    out.width = w;
    out.height = h;
    out.channels = channels;
    const std::size_t size = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(channels);
    out.pixels.assign(pixels, pixels + size);
    stbi_image_free(pixels);
    return true;
}

bool loadImage16(const std::filesystem::path& path, Image16& out, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!readAll(path, bytes, error))
        return false;
    int w = 0, h = 0, n = 0;
    stbi_us* pixels = stbi_load_16_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                               static_cast<int>(bytes.size()), &w, &h, &n, 1);
    if (pixels == nullptr)
    {
        error = std::format("'{}': cannot decode PNG ({})", core::pathToUtf8(path), stbi_failure_reason());
        return false;
    }
    out.width = w;
    out.height = h;
    const std::size_t size = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    out.pixels.assign(pixels, pixels + size);
    stbi_image_free(pixels);
    return true;
}

bool writePng8(const std::filesystem::path& path, const Image8& image, std::string& error)
{
    if (image.width <= 0 || image.height <= 0 || image.channels < 1 || image.channels > 4 ||
        image.pixels.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) *
                                    static_cast<std::size_t>(image.channels))
    {
        error = "writePng8: bad image";
        return false;
    }
    std::vector<std::byte> png;
    if (stbi_write_png_to_func(&appendToVector, &png, image.width, image.height, image.channels,
                               image.pixels.data(), image.width * image.channels) == 0)
    {
        error = "writePng8: encoder failed";
        return false;
    }
    return vo::assets::writeFileBytes(path, png, error);
}

} // namespace vo::tools::assetc
