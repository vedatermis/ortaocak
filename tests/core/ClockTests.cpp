#include "shared/core/Clock.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace std::chrono_literals;

TEST_CASE("Monotonic clock never goes backwards", "[clock]")
{
    auto prev = vo::core::monoNow();
    auto prevNs = vo::core::monoNowNs();
    for (int i = 0; i < 100000; ++i)
    {
        const auto now = vo::core::monoNow();
        const auto nowNs = vo::core::monoNowNs();
        REQUIRE(now >= prev);
        REQUIRE(nowNs >= prevNs);
        prev = now;
        prevNs = nowNs;
    }
}

TEST_CASE("Stopwatch measures elapsed time", "[clock]")
{
    vo::core::Stopwatch sw;
    std::this_thread::sleep_for(20ms);
    const auto elapsed = sw.elapsed();
    CHECK(elapsed >= 20ms);
    CHECK(elapsed < 5s); // sanity upper bound, generous for loaded CI machines
    CHECK(sw.elapsedMs() >= 20);
    CHECK(sw.elapsedSeconds() >= 0.020);
}

TEST_CASE("Stopwatch lap restarts the measurement", "[clock]")
{
    vo::core::Stopwatch sw;
    std::this_thread::sleep_for(15ms);
    const auto first = sw.lap();
    CHECK(first >= 15ms);
    // Immediately after a lap, elapsed restarts near zero (well below the first lap).
    CHECK(sw.elapsed() < first);

    sw.reset();
    CHECK(sw.elapsed() < 1s);
}

TEST_CASE("toSeconds converts durations", "[clock]")
{
    CHECK(vo::core::toSeconds(std::chrono::duration_cast<vo::core::MonoClock::duration>(1500ms)) == 1.5);
}
