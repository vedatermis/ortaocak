#include "client/net/RequestTracker.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vo::client::net;

TEST_CASE("RequestTracker completes the oldest matching request", "[client][net][requests]")
{
    RequestTracker t;
    REQUIRE(t.empty());
    t.add(0x0111, Millis(1000), Millis(500));
    t.add(0x0113, Millis(1100), Millis(500));
    t.add(0x0111, Millis(1200), Millis(500));
    REQUIRE(t.size() == 3);
    REQUIRE(t.isPending(0x0113));
    REQUIRE_FALSE(t.isPending(0x0121));

    REQUIRE_FALSE(t.complete(0x0121)); // unsolicited opcode
    REQUIRE(t.complete(0x0111));       // the first LIST answer
    REQUIRE(t.size() == 2);
    REQUIRE(t.isPending(0x0111));
    // The remaining LIST request started at 1200, the CREATE at 1100.
    REQUIRE_FALSE(t.expired(Millis(1599)).has_value());
    REQUIRE(t.expired(Millis(1600)) == std::uint16_t{0x0113});
    REQUIRE(t.complete(0x0113));
    REQUIRE_FALSE(t.expired(Millis(1650)).has_value());
    REQUIRE(t.expired(Millis(1700)) == std::uint16_t{0x0111});
    t.clear();
    REQUIRE(t.empty());
    REQUIRE_FALSE(t.expired(Millis(99999)).has_value());
}

TEST_CASE("PingTracker schedules pings and measures RTT", "[client][net][ping]")
{
    PingTracker p(Millis(5000), Millis(20000));
    REQUIRE_FALSE(p.pingDue(Millis(0)).has_value()); // not connected yet

    p.reset(Millis(10000));
    const auto first = p.pingDue(Millis(10000)); // due immediately after connecting
    REQUIRE(first.has_value());
    REQUIRE(*first == 10000u);
    REQUIRE_FALSE(p.pingDue(Millis(14999)).has_value());
    REQUIRE_FALSE(p.lastRttMs().has_value());

    const auto rtt = p.onPong(*first, Millis(10040));
    REQUIRE(rtt.has_value());
    REQUIRE(*rtt == Catch::Approx(40.0));
    REQUIRE(p.smoothedRttMs().value() == Catch::Approx(40.0));
    REQUIRE(p.outstanding() == 0);
    REQUIRE_FALSE(p.onPong(*first, Millis(10050)).has_value()); // duplicate echo
    REQUIRE_FALSE(p.onPong(12345, Millis(10050)).has_value());  // never sent

    const auto second = p.pingDue(Millis(15000));
    REQUIRE(second.has_value());
    REQUIRE(p.onPong(*second, Millis(15080)).value() == Catch::Approx(80.0));
    REQUIRE(p.lastRttMs().value() == Catch::Approx(80.0));
    REQUIRE(p.smoothedRttMs().value() == Catch::Approx(50.0)); // 40 * 0.75 + 80 * 0.25
}

TEST_CASE("PingTracker times out when PONGs stop", "[client][net][ping]")
{
    PingTracker p(Millis(5000), Millis(20000));
    p.reset(Millis(0));
    REQUIRE(p.pingDue(Millis(0)).has_value());
    REQUIRE(p.pingDue(Millis(5000)).has_value());
    REQUIRE(p.pingDue(Millis(10000)).has_value());
    REQUIRE_FALSE(p.timedOut(Millis(19999)));
    REQUIRE(p.timedOut(Millis(20000)));
    REQUIRE(p.outstanding() == 3);

    // An answer to a later ping clears the older outstanding ones too.
    p.reset(Millis(0));
    const auto a = p.pingDue(Millis(0));
    const auto b = p.pingDue(Millis(5000));
    REQUIRE((a && b));
    REQUIRE(p.onPong(*b, Millis(5010)).has_value());
    REQUIRE(p.outstanding() == 0);
    REQUIRE_FALSE(p.timedOut(Millis(30000)));
}

TEST_CASE("PingTracker stamps stay distinct and survive 32-bit wrap", "[client][net][ping]")
{
    PingTracker p(Millis(0), Millis(20000)); // interval 0: a ping is due on every call
    const Millis t(0x1'0000'0005LL);           // beyond 32 bits
    p.reset(t);
    const auto s1 = p.pingDue(t);
    const auto s2 = p.pingDue(t);
    REQUIRE((s1 && s2));
    REQUIRE(*s1 == 5u);
    REQUIRE(*s1 != *s2);
    REQUIRE(p.onPong(*s2, t + Millis(7)).value() == Catch::Approx(7.0));
}
