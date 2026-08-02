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
 * Tests for Stopwatch, which times a print job.
 *
 * Stopwatch reads the millisecond clock directly, so these tests assert its state
 * machine and the return values that say whether a transition was accepted, rather
 * than elapsed times — a test that asserted a specific duration would be timing
 * dependent. Where duration is checked it is checked as a bound, not a value.
 *
 * Every transition is asserted on its own statement: putting two calls in one
 * expression leaves their order unspecified.
 */

#include "../test/unit_tests.h"
#include "src/libs/stopwatch.h"
#include "src/inc/MarlinConfig.h"
#include "../support/test_clock.h"

MARLIN_TEST(stopwatch, starts_stopped) {
  Stopwatch sw;
  sw.reset();
  TEST_ASSERT_FALSE(sw.isRunning());
  TEST_ASSERT_FALSE(sw.isPaused());
  TEST_ASSERT_EQUAL(0, sw.duration());
}

MARLIN_TEST(stopwatch, start_runs_it) {
  Stopwatch sw;
  sw.reset();
  TEST_ASSERT_TRUE(sw.start());
  TEST_ASSERT_TRUE(sw.isRunning());
  TEST_ASSERT_FALSE(sw.isPaused());
}

// A transition returns whether it was accepted, so a repeat is refused rather than
// silently restarting the timer.
MARLIN_TEST(stopwatch, starting_twice_is_refused) {
  Stopwatch sw;
  sw.reset();
  TEST_ASSERT_TRUE(sw.start());
  TEST_ASSERT_FALSE(sw.start());
  TEST_ASSERT_TRUE(sw.isRunning());
}

MARLIN_TEST(stopwatch, pause_and_resume) {
  Stopwatch sw;
  sw.reset();
  sw.start();

  TEST_ASSERT_TRUE(sw.pause());
  TEST_ASSERT_TRUE(sw.isPaused());
  TEST_ASSERT_FALSE(sw.isRunning());

  TEST_ASSERT_FALSE(sw.pause());        // already paused

  TEST_ASSERT_TRUE(sw.start());         // start resumes from paused
  TEST_ASSERT_TRUE(sw.isRunning());
  TEST_ASSERT_FALSE(sw.isPaused());
}

MARLIN_TEST(stopwatch, stop_ends_it) {
  Stopwatch sw;
  sw.reset();
  sw.start();

  TEST_ASSERT_TRUE(sw.stop());
  TEST_ASSERT_FALSE(sw.isRunning());
  TEST_ASSERT_FALSE(sw.isPaused());

  TEST_ASSERT_FALSE(sw.stop());         // already stopped
}

MARLIN_TEST(stopwatch, a_stopped_watch_cannot_be_paused) {
  Stopwatch sw;
  sw.reset();
  sw.start();
  sw.stop();
  TEST_ASSERT_FALSE(sw.pause());
  TEST_ASSERT_FALSE(sw.isPaused());
}

MARLIN_TEST(stopwatch, reset_clears_a_running_watch) {
  Stopwatch sw;
  sw.reset();
  sw.start();
  sw.reset();
  TEST_ASSERT_FALSE(sw.isRunning());
  TEST_ASSERT_FALSE(sw.isPaused());
  TEST_ASSERT_EQUAL(0, sw.duration());
}

// resume() restores a previously accumulated time, so a print resumed after a power
// loss continues counting from where it left off rather than from zero.
MARLIN_TEST(stopwatch, resume_restores_accumulated_time) {
  Stopwatch sw;
  sw.reset();
  sw.resume(1234);
  TEST_ASSERT_TRUE(sw.isRunning());
  TEST_ASSERT_TRUE(sw.duration() >= 1234);
}

// LEGACY-BEHAVIOR: resume() starts the watch only when the restored time is non-zero
// (`if ((accumulator = with_time)) state = RUNNING;`). Resuming a job that had
// accumulated exactly zero milliseconds therefore leaves the watch stopped, and the
// job runs untimed. Power-loss recovery normally restores a non-zero time, so this is
// an edge rather than a live fault.
MARLIN_TEST(stopwatch, resume_with_zero_does_not_start) {
  Stopwatch sw;
  sw.reset();
  sw.resume(0);
  TEST_ASSERT_FALSE(sw.isRunning());
  TEST_ASSERT_EQUAL(0, sw.duration());
}

// duration() is reported in seconds, so a transition timed in microseconds must read as
// zero. These pin the timestamp bookkeeping: if a timestamp were left unset, the
// subtraction would run against zero and report the time since boot instead.
MARLIN_TEST(stopwatch, duration_is_zero_immediately_after_starting) {
  Stopwatch sw;
  sw.reset();
  sw.start();
  TEST_ASSERT_EQUAL(0, sw.duration());
}

MARLIN_TEST(stopwatch, duration_is_zero_immediately_after_pausing) {
  Stopwatch sw;
  sw.reset();
  sw.start();
  sw.pause();
  TEST_ASSERT_EQUAL(0, sw.duration());
}

MARLIN_TEST(stopwatch, duration_is_zero_immediately_after_stopping) {
  Stopwatch sw;
  sw.reset();
  sw.start();
  sw.stop();
  TEST_ASSERT_EQUAL(0, sw.duration());
}

// A stopped watch no longer advances: the same value is reported twice.
MARLIN_TEST(stopwatch, a_stopped_watch_holds_its_duration) {
  Stopwatch sw;
  sw.reset();
  sw.start();
  sw.stop();
  const uint32_t first = sw.duration();
  const uint32_t second = sw.duration();
  TEST_ASSERT_EQUAL(first, second);
}

MARLIN_TEST(stopwatch, reset_clears_accumulated_time) {
  Stopwatch sw;
  sw.reset();
  sw.resume(1234);
  sw.reset();
  TEST_ASSERT_EQUAL(0, sw.duration());
}

// Pausing banks the elapsed time, and starting again continues from it.
MARLIN_TEST(stopwatch, pausing_and_restarting_keeps_the_banked_time) {
  Stopwatch sw;
  sw.reset();
  sw.resume(600);
  sw.pause();
  TEST_ASSERT_EQUAL(600, sw.duration() - (millis() / 1000));
  sw.start();
  TEST_ASSERT_TRUE(sw.duration() >= 600);
}

// LEGACY-BEHAVIOR: resume() zeroes startTimestamp via reset() and then marks the watch
// RUNNING without setting it again, so duration() computes millis() - 0 and adds the
// controller's uptime to the restored time. A print resumed after a power loss is
// therefore reported as older than it is, by however long the board has been powered.
// Pinned as an exact relationship so it cannot drift unnoticed.
MARLIN_TEST(stopwatch, resume_adds_the_controllers_uptime) {
  Stopwatch sw;
  sw.reset();
  const uint32_t uptime_s = millis() / 1000;
  sw.resume(1234);
  TEST_ASSERT_EQUAL(1234 + uptime_s, sw.duration());
}


/**
 * Advancing time.
 *
 * duration() is reported in seconds, so the transitions above all read as zero and
 * leave the timing arithmetic itself unasserted. The native HAL clock can be
 * accelerated, which lets a test cover seconds of simulated time in a millisecond of
 * real time without a sleep in the suite and without changing Stopwatch itself.
 */
namespace {

  // Kept as a name the scenarios below read well with; the mechanism lives in
  // support/test_clock.h and differs per HAL.
  struct AcceleratedClock {
    TestClock scope;   // accelerates the clock for as long as the test runs
    static void advance_seconds(const uint32_t s) { TestClock::advance_seconds(s); }
  };

}

MARLIN_TEST(stopwatch, duration_advances_while_running) {
  [[maybe_unused]] AcceleratedClock clock;   // no-op under the test HAL, where time is exact
  Stopwatch sw;
  sw.reset();
  sw.start();
  AcceleratedClock::advance_seconds(3);
  TEST_ASSERT_TRUE(sw.duration() >= 2);
}

MARLIN_TEST(stopwatch, duration_stops_advancing_when_paused) {
  [[maybe_unused]] AcceleratedClock clock;   // no-op under the test HAL, where time is exact
  Stopwatch sw;
  sw.reset();
  sw.start();
  AcceleratedClock::advance_seconds(3);
  sw.pause();

  const uint32_t at_pause = sw.duration();
  AcceleratedClock::advance_seconds(3);
  TEST_ASSERT_EQUAL(at_pause, sw.duration());
}

MARLIN_TEST(stopwatch, duration_stops_advancing_when_stopped) {
  [[maybe_unused]] AcceleratedClock clock;   // no-op under the test HAL, where time is exact
  Stopwatch sw;
  sw.reset();
  sw.start();
  AcceleratedClock::advance_seconds(3);
  sw.stop();

  const uint32_t at_stop = sw.duration();
  AcceleratedClock::advance_seconds(3);
  TEST_ASSERT_EQUAL(at_stop, sw.duration());
}

// Time spent paused is not counted, but time before the pause is kept.
MARLIN_TEST(stopwatch, a_pause_does_not_count_towards_the_duration) {
  [[maybe_unused]] AcceleratedClock clock;   // no-op under the test HAL, where time is exact
  Stopwatch sw;
  sw.reset();
  sw.start();
  AcceleratedClock::advance_seconds(4);
  sw.pause();
  const uint32_t banked = sw.duration();

  AcceleratedClock::advance_seconds(10);   // paused: should not count
  sw.start();
  TEST_ASSERT_TRUE(sw.duration() >= banked);
  TEST_ASSERT_TRUE(sw.duration() < banked + 5);
}
