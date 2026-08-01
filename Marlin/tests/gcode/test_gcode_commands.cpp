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
 * Tests for G-code commands that change a setting.
 *
 * These go in through the front door: a command line is parsed and dispatched exactly
 * as one arriving from a host would be, and the assertion is on the setting it was
 * supposed to change. That exercises the dispatcher and the handler together, and
 * exercises the parser indirectly — which is where a utility's coverage should come
 * from once it has real callers.
 */

#include "../test/unit_tests.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/core/serial.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/temperature.h"
#include <string.h>

namespace {

  /**
   * Report-producing commands would otherwise hang the test binary.
   *
   * The native HAL's serial write busy-waits for room in a 128-byte transmit buffer
   * (HAL/LINUX/include/serial.h). In the simulator that buffer is drained by the UI; in
   * the unit test binary nothing drains it, so the first report that overflows it spins
   * forever. Marking the port as having no host attached — a state the firmware already
   * understands — makes write() return immediately instead.
   *
   * This does not change what the handler does, only where its output goes. A test that
   * needs to assert on the output would have to drain the buffer as it fills.
   */
  struct NoHostAttached {
    bool was;
    NoHostAttached() { was = MYSERIAL1.host_connected; MYSERIAL1.host_connected = false; }
    ~NoHostAttached() { MYSERIAL1.host_connected = was; }
  };

  // Send one command line, the way the queue would.
  void host_sends(const char * const line) {
    NoHostAttached quiet;
    static char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);   // true: skip the "ok" acknowledgement
  }

}

// M111 sets the debug flags used by the logging macros.
MARLIN_TEST(gcode_commands, M111_sets_debug_flags) {
  const uint8_t was = marlin_debug_flags;

  host_sends("M111 S0");
  TEST_ASSERT_EQUAL(0, marlin_debug_flags);

  host_sends("M111 S7");
  TEST_ASSERT_EQUAL(7, marlin_debug_flags);

  // Without S the flags are reported, not changed.
  host_sends("M111");
  TEST_ASSERT_EQUAL(7, marlin_debug_flags);

  marlin_debug_flags = was;
}

#if HAS_SOFTWARE_ENDSTOPS

  MARLIN_TEST(gcode_commands, M211_enables_and_disables_soft_endstops) {
    const bool was = motion.soft_endstop._enabled;

    host_sends("M211 S0");
    TEST_ASSERT_FALSE(motion.soft_endstop._enabled);

    host_sends("M211 S1");
    TEST_ASSERT_TRUE(motion.soft_endstop._enabled);

    // Without S the state is reported, not changed.
    host_sends("M211");
    TEST_ASSERT_TRUE(motion.soft_endstop._enabled);

    motion.soft_endstop._enabled = was;
  }

#endif

MARLIN_TEST(gcode_commands, M220_sets_the_feedrate_percentage) {
  const int16_t was = motion.feedrate_percentage;

  host_sends("M220 S50");
  TEST_ASSERT_EQUAL(50, motion.feedrate_percentage);

  host_sends("M220 S200");
  TEST_ASSERT_EQUAL(200, motion.feedrate_percentage);

  // Without any parameter the value is reported, not changed.
  host_sends("M220");
  TEST_ASSERT_EQUAL(200, motion.feedrate_percentage);

  motion.feedrate_percentage = was;
}

// B stores the current rate, R restores it — so a script can change the speed and put
// it back without knowing what it was.
MARLIN_TEST(gcode_commands, M220_backs_up_and_restores_the_feedrate) {
  const int16_t was = motion.feedrate_percentage;

  host_sends("M220 S100");
  host_sends("M220 B");            // remember 100
  host_sends("M220 S25");
  TEST_ASSERT_EQUAL(25, motion.feedrate_percentage);

  host_sends("M220 R");            // restore
  TEST_ASSERT_EQUAL(100, motion.feedrate_percentage);

  motion.feedrate_percentage = was;
}

#if HAS_EXTRUDERS

  MARLIN_TEST(gcode_commands, M221_sets_the_flow_percentage) {
    const int16_t was = planner.flow_percentage[0];

    host_sends("M221 S75");
    TEST_ASSERT_EQUAL(75, planner.flow_percentage[0]);

    host_sends("M221 S100");
    TEST_ASSERT_EQUAL(100, planner.flow_percentage[0]);

    planner.set_flow(0, was);
  }

#endif

#if ENABLED(PREVENT_COLD_EXTRUSION)

  MARLIN_TEST(gcode_commands, M302_sets_the_cold_extrusion_limit) {
    const celsius_t was_temp = thermalManager.extrude_min_temp;
    const bool was_allowed = thermalManager.allow_cold_extrude;

    host_sends("M302 S180");
    TEST_ASSERT_EQUAL(180, thermalManager.extrude_min_temp);
    TEST_ASSERT_FALSE(thermalManager.allow_cold_extrude);

    // P1 allows cold extrusion outright.
    host_sends("M302 P1");
    TEST_ASSERT_TRUE(thermalManager.allow_cold_extrude);

    host_sends("M302 P0");
    TEST_ASSERT_FALSE(thermalManager.allow_cold_extrude);

    // S0 means "no minimum", which is the same as allowing it.
    host_sends("M302 S0");
    TEST_ASSERT_EQUAL(0, thermalManager.extrude_min_temp);
    TEST_ASSERT_TRUE(thermalManager.allow_cold_extrude);

    thermalManager.extrude_min_temp = was_temp;
    thermalManager.allow_cold_extrude = was_allowed;
  }

#endif

// An unknown command must not be mistaken for a known one.
MARLIN_TEST(gcode_commands, an_unknown_command_changes_nothing) {
  const int16_t was = motion.feedrate_percentage;
  host_sends("M220 S123");
  TEST_ASSERT_EQUAL(123, motion.feedrate_percentage);

  host_sends("M99999 S50");
  TEST_ASSERT_EQUAL(123, motion.feedrate_percentage);

  motion.feedrate_percentage = was;
}
