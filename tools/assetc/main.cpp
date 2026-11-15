// assetc - offline asset compiler (glTF/PNG -> .vmesh/.vanim/.vtex).
// Skeleton: prints name and version. Implementation belongs to the asset work package (M5).

#include "shared/core/Version.h"

#include <cstdio>

int main()
{
    std::printf("assetc %s (%s %s)\n", vo::version::kString, vo::version::kProductName, vo::version::kBuildConfig);
    return 0;
}
