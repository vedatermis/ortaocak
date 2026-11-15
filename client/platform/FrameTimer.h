// Per-frame timing: delta time, frame counter and a once-per-second FPS estimate.
#pragma once

#include "shared/core/Clock.h"

#include <cstdint>

namespace vo::client::platform {

class FrameTimer
{
public:
    // Call once per frame; returns the time since the previous tick in seconds (clamped to 0.25 s so a
    // debugger pause or window drag doesn't produce a huge simulation step).
    double tick() noexcept
    {
        double dt = vo::core::toSeconds(m_frame.lap());
        if (dt > kMaxDelta)
            dt = kMaxDelta;
        ++m_frameCount;
        ++m_framesInWindow;
        const double windowSeconds = m_fpsWindow.elapsedSeconds();
        if (windowSeconds >= 1.0)
        {
            m_fps = static_cast<double>(m_framesInWindow) / windowSeconds;
            m_framesInWindow = 0;
            m_fpsWindow.reset();
            m_fpsUpdated = true;
        }
        return dt;
    }

    [[nodiscard]] std::uint64_t frameCount() const noexcept { return m_frameCount; }
    [[nodiscard]] double fps() const noexcept { return m_fps; }

    // True once after each FPS update.
    bool consumeFpsUpdate() noexcept
    {
        const bool updated = m_fpsUpdated;
        m_fpsUpdated = false;
        return updated;
    }

private:
    static constexpr double kMaxDelta = 0.25;

    vo::core::Stopwatch m_frame;
    vo::core::Stopwatch m_fpsWindow;
    std::uint64_t m_frameCount = 0;
    std::uint64_t m_framesInWindow = 0;
    double m_fps = 0.0;
    bool m_fpsUpdated = false;
};

} // namespace vo::client::platform
