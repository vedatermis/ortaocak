// PNG export of a captured back buffer via WIC. COM must be initialised on the calling thread.
#pragma once

#include "client/render/RenderDevice.h"

#include <filesystem>
#include <string>

namespace vo::client::render {

bool writePng(const std::filesystem::path& path, const CapturedImage& image, std::string& error);

} // namespace vo::client::render
