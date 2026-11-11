#include "rx_guard_policy.hpp"

using namespace rxguard;

constexpr uint64_t failedFirstPoll = 0x02800002ULL;
constexpr uint64_t acknowledged = 0x00800002ULL;
constexpr uint8_t locked = 1;
constexpr uint8_t detectorEnabled = 4;

static_assert(canClearStartupClockEvent(failedFirstPoll, locked, detectorEnabled),
              "the first POLL failure may be acknowledged only at startup with live lock");
static_assert(!settledStartupClock(failedFirstPoll, locked),
              "a recurring or unacknowledged loss still blocks readiness");
static_assert(settledStartupClock(acknowledged, locked),
              "an acknowledged startup event with live lock is ready");
static_assert(!canClearStartupClockEvent(failedFirstPoll | RF_PLL_LOSS, locked, detectorEnabled),
              "an RF PLL loss cannot be acknowledged as a clock startup event");
static_assert(!canClearStartupClockEvent(failedFirstPoll, 0, detectorEnabled),
              "an unlocked clock blocks readiness");
static_assert(!canClearStartupClockEvent(failedFirstPoll, locked, 0),
              "a disabled PLL lock-loss detector blocks readiness");
static_assert(!canClearStartupClockEvent(MASK40, locked, detectorEnabled),
              "an invalid status read blocks readiness");
static_assert(!canClearStartupClockEvent(failedFirstPoll, 0xff, detectorEnabled),
              "an invalid live-lock read blocks readiness");
static_assert(!canClearStartupClockEvent(failedFirstPoll, locked, 0xff),
              "an invalid detector read blocks readiness");
static_assert(!settledStartupClock(acknowledged, 0) && !settledStartupClock(MASK40, locked),
              "loss of live lock or status integrity after acknowledgment blocks readiness");
static_assert(classify(failedFirstPoll) == Action::ClockFault &&
                  classify(acknowledged | RF_PLL_LOSS) == Action::ClockFault,
              "runtime clock and RF PLL loss both remain faults");
