// `assetc build <assets_src> <assets>`: incremental conversion of a whole source tree.
//
// Rules (sorted path order, deterministic):
//   1. **/*.png copied verbatim, except under a directory named `previews` and in directories that
//      contain a *.terrain.toml (terrain inputs are baked into the .vterrain).
//   2. **/*.glb|*.gltf -> <rel>/<stem>.vmesh, plus <rel>/<stem>/<clip>.vanim when it has animations
//      (sidecar <stem>.anims.toml).
//   3. **/<name>.terrain.toml -> <rel>/<name>.vterrain; afterwards every placement mesh and layer
//      texture must exist in the output tree.
// Up-to-date check: a stamp file per source under <assets>/.assetc/ lists inputs and outputs; an item
// is skipped when the stamp is newer than every input and the tool, and all outputs exist.
#pragma once

#include <filesystem>
#include <string>

namespace vo::tools::assetc {

struct BuildOptions
{
    std::filesystem::file_time_type toolTime{}; // modification time of assetc itself (rebuild if newer)
    bool force = false;
    bool quantizeRotations = false;
};

struct BuildStats
{
    int converted = 0;
    int copied = 0;
    int skipped = 0;
    int failed = 0;
};

// Returns true if every item succeeded. Errors are logged; conversion continues after a failure.
[[nodiscard]] bool buildAssets(const std::filesystem::path& sourceRoot, const std::filesystem::path& outputRoot,
                               const BuildOptions& options, BuildStats& stats);

} // namespace vo::tools::assetc
