#pragma once

#include <chrono>
#include <functional>

namespace pzynet
{

/// Monotonic clock used throughout. Protocol engines never read it themselves: the current time is
/// always passed in, which keeps them deterministic and testable with a fake clock.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

/// Where a driver takes the current time from; an empty source means Clock::now().
using ClockSource = std::function<TimePoint()>;

} // namespace pzynet
