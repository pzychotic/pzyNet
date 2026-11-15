#pragma once

#include <chrono>

namespace pzynet
{

/// Monotonic clock used throughout. Protocol engines never read it themselves: the current time is
/// always passed in, which keeps them deterministic and testable with a fake clock.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

} // namespace pzynet
