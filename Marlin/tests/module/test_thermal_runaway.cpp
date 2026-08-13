/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2024 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/**
 * The state machine that decides whether a printer catches fire.
 *
 * A heater whose thermistor has fallen out of the block reads cold for ever, so the
 * control loop drives it to full power and keeps it there. Thermal runaway detection is
 * the only thing standing between that and a fire, and its whole job is a judgement about
 * *time*: the temperature is allowed to sit below target for a while, because that is what
 * a cold draught or a fresh filament change looks like, and not for longer, because that is
 * what a detached thermistor looks like.
 *
 * Both errors are expensive and they pull in opposite directions. Tripping early stops a
 * print that was fine. Tripping late — or not at all — is the failure the feature exists to
 * prevent. So the assertions here are about *when* the transition happens rather than that
 * it happens: one degree inside the hysteresis and one degree outside it, one millisecond
 * before the period elapses and one after.
 *
 * `tr_state_machine_t::run()` takes the current temperature, the target, the heater, the
 * period and the hysteresis as arguments and keeps its own state, so it can be driven
 * directly with no heater, no sensor and no control loop in the way. That is unusual in this
 * firmware and worth using: every assertion below is about the decision itself rather than
 * about a machine that happens to be arranged to provoke it.
 *
 * What is *not* here is the trip. `TRRunaway` calls `_TEMP_ERROR`, which in this build ends
 * in `Marlin::kill()` and never returns, so no assertion can run after it — the third
 * category in the survivor taxonomy, a blocked seam rather than a gap. Everything up to and
 * including the last state before the trip is asserted, from both sides.
 *
 * Test-HAL only: the period is tens of seconds and the test steps over it, which needs time
 * that moves when a test says so.
 */

#include "../test/unit_tests.h"
#include "../support/test_clock.h"
#include "src/module/temperature.h"

#if HAS_THERMAL_PROTECTION && ENABLED(THERMAL_PROTECTION_HOTENDS)

namespace {

  // The hotend's own configured limits, so the test states the machine's rules rather
  // than a pair of numbers that happen to agree with them.
  constexpr uint16_t PERIOD_S = THERMAL_PROTECTION_PERIOD;
  constexpr celsius_float_t HYSTERESIS = THERMAL_PROTECTION_HYSTERESIS;
  constexpr celsius_float_t TARGET = 200.0f;

  using TR = Temperature::tr_state_machine_t;

  /**
   * A watchdog that has not seen anything yet.
   *
   * `running_temp` is the one member of `tr_state_machine_t` with no default initialiser —
   * `timer` and `state` have one, it does not. The firmware's own instances are a static
   * array and therefore zeroed, so this is latent there; anywhere else the field starts as
   * whatever was on the stack. That matters because the arming test is
   * `running_temp != target`: a fresh watchdog whose garbage happens to equal the target
   * never arms at all, and the heater it was protecting is watched by nothing.
   *
   * Found by these tests, one of which failed intermittently until it was understood.
   * Recorded as defect #49 rather than fixed, and zeroed here so the tests below are about
   * the state machine rather than about the stack.
   */
  TR fresh() {
    TR m;
    m.running_temp = 0.0f;
    return m;
  }

  // One pass of the watchdog for heater 0, with this build's period and hysteresis.
  void watch(TR &m, const celsius_float_t current, const celsius_float_t target = TARGET) {
    m.run(current, target, H_E0, PERIOD_S, HYSTERESIS);
  }

  // A machine that has heated up and settled: the state every interesting case starts from.
  TR settled_at_target() {
    TR m = fresh();
    watch(m, 20.0f);                    // a target appears — start watching the climb
    watch(m, TARGET);                   // ...and it arrives
    return m;
  }

}

// ---------------------------------------------------------------------------
// Arming
// ---------------------------------------------------------------------------

/**
 * With no target there is nothing to protect, and the watchdog stays out of the way.
 *
 * A machine sitting cold with its heaters off is not in trouble however far its
 * temperature is from zero, and a watchdog that armed itself here would trip on every
 * printer that had just been switched on.
 */
MARLIN_TEST(thermal_runaway, no_target_means_nothing_to_watch) {
  TR m = fresh();

  watch(m, 20.0f, 0.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRInactive, m.state,
    "a heater with no target should not be watched");

  // ...and staying cold for a long time does not change that.
  TestClock::advance_seconds(PERIOD_S * 3);
  watch(m, 20.0f, 0.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRInactive, m.state,
    "time passing should not arm a watchdog that has nothing to watch");
}

/**
 * A target arms the watchdog, and the first phase is the climb.
 *
 * Heating from cold necessarily spends a long time far below target, which is exactly the
 * shape of a runaway — so the first phase has to be a separate state that does not judge
 * the gap at all. Treating the climb as a stable period is the classic false positive.
 */
MARLIN_TEST(thermal_runaway, a_target_starts_the_watch_at_the_climb) {
  TR m = fresh();

  watch(m, 20.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRFirstHeating, m.state,
    "setting a target should start watching the climb");

  // Still climbing, still a long way off, and a whole period has gone by: not a runaway.
  TestClock::advance_seconds(PERIOD_S * 2);
  watch(m, 100.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRFirstHeating, m.state,
    "a slow climb is not a runaway, however long it takes");
}

/**
 * Any target at all is a heater that is on, and the boundary is zero.
 *
 * The other side of `no_target_means_nothing_to_watch`: the test that decides whether to
 * arm is `target > 0`, so a target of one degree must arm it. Showing only that 200 arms it
 * would pass with the threshold anywhere below 200 — a machine that ignored every target
 * under, say, fifty degrees would watch nothing while the bed was warming.
 *
 * A room-temperature machine is already past a one degree target, so arming here lands
 * straight in `TRStable` rather than in the climb. That is the assertion: watching, and
 * watching immediately. Not arming would leave it `TRInactive`.
 */
MARLIN_TEST(thermal_runaway, the_smallest_target_above_zero_still_arms_the_watch) {
  TR m = fresh();

  watch(m, 20.0f, 1.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, m.state,
    "a one degree target is still a heater that is switched on");
}

/**
 * Reaching the target ends the climb — and reaching it is `>=`, not `>`.
 *
 * The boundary matters because a heater that settles exactly on its target would otherwise
 * stay in the climb phase for ever, and the climb phase watches nothing. A printer whose
 * thermistor then fell out would be unprotected for the rest of the job.
 */
MARLIN_TEST(thermal_runaway, arriving_at_the_target_ends_the_climb) {
  TR below = fresh();
  watch(below, 20.0f);
  watch(below, TARGET - 1.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRFirstHeating, below.state,
    "one degree short is still the climb");

  TR at = fresh();
  watch(at, 20.0f);
  watch(at, TARGET);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, at.state,
    "landing exactly on the target should end the climb");
}

// ---------------------------------------------------------------------------
// Watching
// ---------------------------------------------------------------------------

/**
 * Inside the hysteresis nothing is wrong, however long it goes on.
 *
 * A heater cycles around its target by design, and the hysteresis is the width of that
 * cycle. Every pass inside it re-arms the clock, so a machine that is merely oscillating
 * never accumulates any elapsed time at all.
 */
MARLIN_TEST(thermal_runaway, drifting_within_the_hysteresis_is_not_a_fault) {
  TR m = settled_at_target();

  for (uint8_t i = 0; i < 5; i++) {
    TestClock::advance_seconds(PERIOD_S);       // a full period between each pass
    watch(m, TARGET - HYSTERESIS);              // ...at the very edge of allowed drift
    TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, m.state,
      "a heater at the edge of its hysteresis is not running away");
  }
}

/**
 * The hysteresis is a boundary, and it is bracketed from both sides — on the deadline.
 *
 * This is the assertion the file needed and did not have at first. Every state-only test
 * here reads `TRStable` on both sides of the boundary, because the state does not change
 * until the trip and the trip is unreachable; so an implementation that measured the gap
 * completely differently would pass all of them. Replacing `running_temp - current` with
 * `running_temp / current` survived every one, and it is not a subtle mutant: it turns a
 * 50-degree shortfall into 1.33 and reports a runaway as perfectly healthy.
 *
 * What separates them is `timer` — the deadline the decision is carried in. Inside the
 * hysteresis it is pushed forward on every pass, so the count can never run out; outside it
 * the deadline stands still and the clock runs down. That is the whole judgement, and it is
 * observable one call at a time rather than only at the trip.
 */
MARLIN_TEST(thermal_runaway, falling_past_the_hysteresis_stops_pushing_the_deadline_back) {
  // At the edge: still inside, so the deadline moves with every pass.
  TR edge = settled_at_target();
  watch(edge, TARGET - HYSTERESIS);
  const millis_t edge_deadline = edge.timer;

  TestClock::advance_seconds(PERIOD_S / 2);
  watch(edge, TARGET - HYSTERESIS);
  TEST_ASSERT_TRUE_MESSAGE(edge.timer > edge_deadline,
    "a heater inside its hysteresis should have its deadline pushed back on every pass");
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, edge.state, "and should still be stable");

  // One degree further out: the deadline is left where it was and the count runs down.
  TR falling = settled_at_target();
  watch(falling, TARGET - HYSTERESIS);
  const millis_t falling_deadline = falling.timer;

  TestClock::advance_seconds(PERIOD_S / 2);
  watch(falling, TARGET - HYSTERESIS - 1.0f);
  TEST_ASSERT_EQUAL_MESSAGE(falling_deadline, falling.timer,
    "a heater outside its hysteresis should not have its deadline pushed back");
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, falling.state,
    "falling past the hysteresis should start the count, not trip immediately");
}

/**
 * The deadline is the configured period away, not some other interval.
 *
 * "It was pushed back" is satisfied by any positive amount, including one that would let a
 * detached thermistor run for an hour. The distance is the safety property: the machine
 * allows exactly `THERMAL_PROTECTION_PERIOD` and no more.
 */
MARLIN_TEST(thermal_runaway, the_deadline_is_one_configured_period_away) {
  TR m = settled_at_target();

  const millis_t before = millis();
  watch(m, TARGET);

  TEST_ASSERT_EQUAL_MESSAGE(before + SEC_TO_MS(PERIOD_S), m.timer,
    "the allowance should be exactly the configured thermal protection period");
}

/**
 * The trip does not come early — bracketed against the period, one pass either side.
 *
 * This is the false positive the feature has to avoid: a print abandoned because the
 * nozzle spent thirty-nine seconds of a forty-second allowance below target. The heater
 * here is a long way from its target for almost the whole period and the answer is still
 * "not yet".
 *
 * The other side of this bracket — that it *does* trip once the period elapses — cannot be
 * asserted in this build: the trip calls `_TEMP_ERROR`, which never returns. See the file
 * comment. What is pinned here is that nothing trips before it should.
 */
MARLIN_TEST(thermal_runaway, a_cold_spell_shorter_than_the_period_is_tolerated) {
  TR m = settled_at_target();

  watch(m, TARGET - 50.0f);                     // well past the hysteresis: the clock starts

  // One second short of the allowance, checked repeatedly on the way.
  for (uint16_t s = 0; s < PERIOD_S - 1; s++) {
    TestClock::advance_seconds(1);
    watch(m, TARGET - 50.0f);
    TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, m.state,
      "the machine should tolerate a cold spell shorter than its configured period");
  }
}

/**
 * Coming back within the hysteresis cancels the count.
 *
 * A draught, an open door, a cold length of filament: the temperature dips and recovers.
 * If the clock kept running through the recovery, a printer would eventually trip on the
 * accumulation of unrelated dips, which is a fault that would look random to its owner.
 */
MARLIN_TEST(thermal_runaway, recovering_cancels_a_count_already_running) {
  TR m = settled_at_target();

  watch(m, TARGET - 50.0f);                     // the clock starts
  TestClock::advance_seconds(PERIOD_S - 1);     // ...and very nearly runs out
  watch(m, TARGET);                             // recovered: the clock is re-armed
  TestClock::advance_seconds(PERIOD_S - 1);     // another almost-full period
  watch(m, TARGET - 50.0f);

  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, m.state,
    "a recovery should cancel the count, not merely pause it");
}

// ---------------------------------------------------------------------------
// Re-arming
// ---------------------------------------------------------------------------

/**
 * Raising the target starts the climb again.
 *
 * The temperature is now legitimately far below where it is going, which is the climb and
 * not a runaway. A watchdog that carried on judging against the new target would trip on
 * every deliberate temperature change.
 */
MARLIN_TEST(thermal_runaway, raising_the_target_starts_a_fresh_climb) {
  TR m = settled_at_target();
  TEST_ASSERT_EQUAL(Temperature::TRStable, m.state);

  watch(m, TARGET, TARGET + 40.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRFirstHeating, m.state,
    "a higher target should be watched as a fresh climb");
}

/**
 * Lowering the target does not, because the heater is already past it.
 *
 * The restart is the same code either way; what separates the two is the comparison that
 * ends the climb. Coming down to a lower target the machine is above it on the first pass,
 * so the climb ends immediately and protection resumes without a gap. That is the useful
 * half: a cool-down that left the watchdog parked in the climb phase would be unprotected.
 */
MARLIN_TEST(thermal_runaway, lowering_the_target_resumes_watching_at_once) {
  TR m = settled_at_target();

  watch(m, TARGET, TARGET - 40.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, m.state,
    "a heater already above a newly lowered target should be watched straight away");
}

/**
 * Turning the heater off disarms the watchdog rather than leaving it counting.
 *
 * A target of zero is a machine that has finished, and its temperature falling to room
 * temperature is the intended outcome. Left armed, the watchdog would report a thermal
 * runaway on every completed print.
 */
MARLIN_TEST(thermal_runaway, clearing_the_target_disarms_the_watch) {
  TR m = settled_at_target();

  watch(m, TARGET, 0.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRInactive, m.state,
    "clearing the target should disarm the watchdog");

  TestClock::advance_seconds(PERIOD_S * 3);
  watch(m, 25.0f, 0.0f);
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRInactive, m.state,
    "and cooling down afterwards is not a fault");
}

/**
 * Each protected heater has its own state, and they do not share a clock.
 *
 * `tr_state_machine` is an array indexed per heater, and a single shared timer would mean
 * one heater's recovery cancelled another's count — the bed's cycling would mask the
 * hotend's runaway indefinitely. Two instances driven out of step is what says otherwise.
 */
MARLIN_TEST(thermal_runaway, two_heaters_are_watched_independently) {
  TR one = settled_at_target();
  TR two = settled_at_target();

  watch(one, TARGET - 50.0f);                   // one starts counting
  TestClock::advance_seconds(PERIOD_S - 1);
  watch(two, TARGET);                           // the other is perfectly happy

  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, one.state, "the falling heater is still stable");
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, two.state, "and so is the healthy one");

  // The healthy heater's pass must not have re-armed the other one's clock: give the
  // falling heater the last second of its allowance and it should still not have recovered.
  TestClock::advance_seconds(1);
  watch(one, TARGET);                           // recovery, so this pass cannot trip
  TEST_ASSERT_EQUAL_MESSAGE(Temperature::TRStable, one.state,
    "a recovered heater is stable whatever the other one has been doing");
}

#endif // HAS_THERMAL_PROTECTION && THERMAL_PROTECTION_HOTENDS
