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
 * Commands that wait.
 *
 * M400, G4 and an arc all finish by spinning on `marlin.idle()` until something else
 * changes — the queue drains, a period elapses, a block frees up. Under the LINUX HAL
 * that spin has nothing to end it in a test build, so none of these could be tested at
 * all. Under the test HAL idle() costs simulated time and interrupts fire inside it, so
 * the wait ends for the same reason it ends on a board.
 *
 * Test-HAL only: these deliberately hang under HAL/LINUX.
 */

#ifdef __PLAT_TEST__

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/stepper.h"
#include "serial_capture.h"
#include <string.h>

namespace {

  // Send one command line the way the queue would. The fixture has already silenced the
  // serial port, so a report cannot fill the transmit buffer and spin.
  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);   // true: skip the "ok" acknowledgement
  }

  // Put the machine at the origin, steppers included, so step counts are absolute.
  void at_the_origin() {
    xyze_pos_t origin = { 0 };
    motion.position = origin;
    planner.set_position_mm(origin);
  }

}

// M400 waits for the queue to drain, and the move really runs while it waits.
MARLIN_TEST(blocking_commands, M400_waits_for_a_queued_move_to_finish) {
  SimulatedMachine machine;
  at_the_origin();

  xyze_pos_t target = { 0 }; target.x = 2.0f;
  TEST_ASSERT_TRUE(planner.buffer_line(target, 10.0f));
  TEST_ASSERT_TRUE(planner.has_blocks_queued());

  host_sends("M400");

  TEST_ASSERT_FALSE(planner.has_blocks_queued());
  TEST_ASSERT_EQUAL(160, stepper.position(X_AXIS));   // 2 mm at 80 steps/mm
}

// With nothing to wait for, M400 does not wait: no time passes.
MARLIN_TEST(blocking_commands, M400_returns_at_once_when_nothing_is_queued) {
  SimulatedMachine machine;
  at_the_origin();
  TEST_ASSERT_FALSE(planner.has_blocks_queued());

  const millis_t before = millis();
  host_sends("M400");

  TEST_ASSERT_EQUAL(before, millis());
  TEST_ASSERT_FALSE(planner.has_blocks_queued());
}

// G4 synchronizes before it dwells, so a queued move is finished first.
MARLIN_TEST(blocking_commands, G4_finishes_queued_moves_before_dwelling) {
  SimulatedMachine machine;
  at_the_origin();

  xyze_pos_t target = { 0 }; target.x = 1.0f;
  TEST_ASSERT_TRUE(planner.buffer_line(target, 10.0f));

  host_sends("G4 P1");

  TEST_ASSERT_FALSE(planner.has_blocks_queued());
  TEST_ASSERT_EQUAL(80, stepper.position(X_AXIS));    // 1 mm at 80 steps/mm
}

// P is milliseconds, and the dwell costs at least that much.
MARLIN_TEST(blocking_commands, G4_P_dwells_for_milliseconds) {
  SimulatedMachine machine;
  at_the_origin();

  const millis_t before = millis();
  host_sends("G4 P50");
  TEST_ASSERT_TRUE(millis() - before >= 50);
}

// S is seconds.
MARLIN_TEST(blocking_commands, G4_S_dwells_for_seconds) {
  SimulatedMachine machine;
  at_the_origin();

  const millis_t before = millis();
  host_sends("G4 S1");
  TEST_ASSERT_TRUE(millis() - before >= 1000);
}

// dwell()'s loop condition is PENDING(now, start, interval) == (now - start) < interval.
// Swapping the last two arguments computes (now - interval) < start instead, which agrees
// with the original for any start large enough that "now - interval" does not itself
// wrap — true of a fixed literal P/S value once the suite's own clock has run for a
// while, which is exactly why a swapped-argument mutant survived every test using one.
// Requesting an interval derived from the clock's own current reading (larger than "now"
// itself, whatever that happens to be) forces the wraparound the swap gets wrong: the
// mutant's condition is false from the very first check, so it returns at once instead
// of waiting.
MARLIN_TEST(blocking_commands, G4_waits_the_full_interval_even_when_it_exceeds_the_clock_so_far) {
  SimulatedMachine machine;
  at_the_origin();

  const millis_t interval = millis() + 100;
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "G4 P%lu", (unsigned long)interval);

  const millis_t before = millis();
  host_sends(cmd);
  TEST_ASSERT_TRUE(millis() - before >= interval);
}

#if ENABLED(HOST_KEEPALIVE_FEATURE)
  // A long dwell holds the handler busy for its whole duration, and host_keepalive() is
  // driven only from inside dwell()'s own idle() loop — nothing here calls it directly.
  // That only reports anything because process_parsed_command() marks busy_state
  // IN_HANDLER before dispatching; deleting that mark leaves busy_state at whatever the
  // previous command left it (NOT_BUSY) and no report goes out at all.
  MARLIN_TEST(blocking_commands, a_long_dwell_reports_busy_to_the_host) {
    SimulatedMachine machine;
    at_the_origin();

    const uint8_t was_interval = gcode.host_keepalive_interval;
    const bool was_paused = gcode.autoreport_paused;
    gcode.host_keepalive_interval = 1;   // seconds
    gcode.set_autoreport_paused(false);

    SerialCapture capture;
    host_sends("G4 P1500");              // longer than one keepalive interval
    TEST_ASSERT_TRUE(capture.saw("busy: processing"));

    gcode.host_keepalive_interval = was_interval;
    gcode.set_autoreport_paused(was_paused);
  }
#endif

#if ENABLED(ARC_SUPPORT)

  /**
   * An arc is longer than the block buffer, so it can only finish if the machine keeps
   * moving while the command is still writing segments. plan_arc() waits for a free slot
   * by calling idle(); if that wait never ended, the command would never return.
   */
  MARLIN_TEST(blocking_commands, G2_runs_an_arc_longer_than_the_block_buffer) {
    SimulatedMachine machine;
    at_the_origin();

    host_sends("G2 X10 Y10 I10 J0 F600");

    TEST_ASSERT_EQUAL_FLOAT(10.0f, motion.position.x);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, motion.position.y);

    TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
    TEST_ASSERT_EQUAL(800, stepper.position(X_AXIS));  // 10 mm at 80 steps/mm
    TEST_ASSERT_EQUAL(800, stepper.position(Y_AXIS));
  }

#endif // ARC_SUPPORT

#endif // __PLAT_TEST__
