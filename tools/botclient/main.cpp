// botclient - headless test client for end-to-end server scenarios.
// Skeleton: prints name and version. Implementation belongs to the test/tools work package (M1).

#include "shared/core/Version.h"

#include <cstdio>

int main()
{
    std::printf("botclient %s (%s %s)\n", vo::version::kString, vo::version::kProductName,
                vo::version::kBuildConfig);
    return 0;
}
