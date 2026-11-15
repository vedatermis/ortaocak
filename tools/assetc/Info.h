// `assetc info`: human-readable header and statistics of a .vmesh, .vanim or .vterrain file.
#pragma once

#include <filesystem>
#include <string>

namespace vo::tools::assetc {

[[nodiscard]] bool describeFile(const std::filesystem::path& path, std::string& text, std::string& error);

} // namespace vo::tools::assetc
