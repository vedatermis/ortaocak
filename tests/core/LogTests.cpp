#include "shared/core/Log.h"

#include <catch2/catch_test_macros.hpp>

using vo::log::Level;

TEST_CASE("Log level parsing", "[log]")
{
    CHECK(vo::log::parseLevel("info") == Level::Info);
    CHECK(vo::log::parseLevel("INFO") == Level::Info);
    CHECK(vo::log::parseLevel("Warning") == Level::Warn);
    CHECK(vo::log::parseLevel("warn") == Level::Warn);
    CHECK(vo::log::parseLevel("trace") == Level::Trace);
    CHECK(vo::log::parseLevel("off") == Level::Off);
    CHECK_FALSE(vo::log::parseLevel("loud").has_value());
    CHECK_FALSE(vo::log::parseLevel("").has_value());
}

TEST_CASE("Log level names round-trip", "[log]")
{
    for (Level lvl : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Fatal, Level::Off})
        CHECK(vo::log::parseLevel(vo::log::toString(lvl)) == lvl);
}

TEST_CASE("Log level filtering", "[log]")
{
    const Level saved = vo::log::level();
    vo::log::setLevel(Level::Warn);
    CHECK_FALSE(vo::log::isEnabled(Level::Info));
    CHECK(vo::log::isEnabled(Level::Warn));
    CHECK(vo::log::isEnabled(Level::Error));
    vo::log::setLevel(Level::Off);
    CHECK_FALSE(vo::log::isEnabled(Level::Fatal));
    CHECK_FALSE(vo::log::isEnabled(Level::Off));
    vo::log::setLevel(saved);
}
