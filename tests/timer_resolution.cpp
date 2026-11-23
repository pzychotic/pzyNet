// Runs the tests at 1 ms timer resolution on Windows.
//
// Windows rounds short waits (WSAPoll, Sleep) up to the system timer tick, ~15.6 ms by default. Tests that
// drive app-driven Peers in turn with update(1ms) then add a whole tick of latency per hop, and a loopback
// round trip plus the receiver's 10 ms acknowledgement delay can exceed the ~30 ms receipt deadline that a
// loopback round-trip estimate gives (RakNet's RTO: 2 * RTT + 4 * deviation + 30 ms). An unreliable send
// with a receipt is then rightly reported lost, although the network delivered it promptly. Real-time
// applications on Windows request 1 ms resolution for the same reason.

#ifdef _WIN32

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

namespace
{

class TimerResolution : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override { timeBeginPeriod(1); }
    void testRunEnded(const Catch::TestRunStats&) override { timeEndPeriod(1); }
};

} // namespace

CATCH_REGISTER_LISTENER(TimerResolution)

#endif
