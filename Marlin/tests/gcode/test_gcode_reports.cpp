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
 * Tests for what commands report back to the host.
 *
 * Most settings commands report their current value when called with no arguments, and
 * a host relies on that text to show the user what the printer is doing. Each test here
 * sets a value and then asks for it back, so the assertion is on the round trip rather
 * than on whatever the printer happened to be holding.
 */

#include "../test/unit_tests.h"
#include "serial_capture.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/temperature.h"
#include <string.h>

namespace {

  // Dispatch a command with nothing capturing, for setting a value up.
  void host_sends(const char * const line) {
    static char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    const bool was = MYSERIAL1.host_connected;
    MYSERIAL1.host_connected = false;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    MYSERIAL1.host_connected = was;
  }

  // Dispatch a command and return what it reported.
  std::string reply_to(const char * const line) {
    static char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    SerialCapture capture;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    return capture.finish();
  }

  bool contains(const std::string &haystack, const char * const needle) {
    return haystack.find(needle) != std::string::npos;
  }

}

MARLIN_TEST(gcode_reports, M220_reports_the_feedrate_it_was_given) {
  const int16_t was = motion.feedrate_percentage;
  host_sends("M220 S150");
  TEST_ASSERT_TRUE(contains(reply_to("M220"), "FR:150%"));
  motion.feedrate_percentage = was;
}

#if HAS_EXTRUDERS
  MARLIN_TEST(gcode_reports, M221_reports_the_flow_it_was_given) {
    const int16_t was = planner.flow_percentage[0];
    host_sends("M221 S80");
    TEST_ASSERT_TRUE(contains(reply_to("M221"), "Flow: 80%"));
    planner.set_flow(0, was);
  }
#endif

MARLIN_TEST(gcode_reports, M110_reports_the_line_number_it_was_given) {
  host_sends("M110 N4321");
  TEST_ASSERT_TRUE(contains(reply_to("M110"), "4321"));
  host_sends("M110 N0");
}

#if HAS_SOFTWARE_ENDSTOPS
  MARLIN_TEST(gcode_reports, M211_reports_whether_soft_endstops_are_on) {
    const bool was = motion.soft_endstop._enabled;

    host_sends("M211 S1");
    const std::string on = reply_to("M211");
    TEST_ASSERT_TRUE(contains(on, "M211 S1"));
    TEST_ASSERT_TRUE(contains(on, "ON"));

    host_sends("M211 S0");
    const std::string off = reply_to("M211");
    TEST_ASSERT_TRUE(contains(off, "M211 S0"));
    TEST_ASSERT_TRUE(contains(off, "OFF"));

    motion.soft_endstop._enabled = was;
  }
#endif

MARLIN_TEST(gcode_reports, M92_reports_the_steps_it_was_given) {
  const float was = planner.settings.axis_steps_per_mm[X_AXIS];
  host_sends("M92 X123");
  TEST_ASSERT_TRUE(contains(reply_to("M92"), "M92 X123.00"));
  planner.settings.axis_steps_per_mm[X_AXIS] = was;
}

/**
 * A direct `M92` query is not a replay: it must not carry the "; Steps per unit:"
 * heading that a saved-settings listing (`M503`) uses to label the section. The two
 * reporting paths share one function distinguished by that flag, so this is what tells
 * them apart.
 */
MARLIN_TEST(gcode_reports, M92_direct_query_omits_the_replay_heading) {
  TEST_ASSERT_TRUE_MESSAGE(reply_to("M92").find(STR_STEPS_PER_UNIT) == std::string::npos,
    "M92 with no arguments answers a live query, not a replay, and should not print the heading");
}

/**
 * `M503` — a saved-settings listing rather than a live query — is the replay path, and
 * is what actually exercises the heading this function can print.
 */
MARLIN_TEST(gcode_reports, M503_report_carries_the_steps_per_unit_heading) {
  TEST_ASSERT_TRUE(contains(reply_to("M503"), STR_STEPS_PER_UNIT));
}

#if HAS_EXTRUDERS
/**
 * The report line for the shared `E` resolution, and the newline that ends it.
 *
 * Asserted together because both are easy to lose silently: the E figure is appended
 * to the same line as X/Y/Z rather than printed by its own statement, and the newline
 * is a separate statement again after that — drop either one and the report still
 * "works", it just runs the next thing on to it or leaves the figure out.
 */
MARLIN_TEST(gcode_reports, M92_report_carries_the_extruder_line_and_ends_it) {
  const float was = planner.settings.axis_steps_per_mm[E_AXIS];
  host_sends("M92 E123");
  const std::string reply = reply_to("M92");
  TEST_ASSERT_TRUE_MESSAGE(contains(reply, "E123.00"),
    "the shared E resolution should be on the same report line as X/Y/Z");
  TEST_ASSERT_TRUE_MESSAGE(!reply.empty() && reply.back() == '\n',
    "the report line must end with a newline, not run into whatever comes after it");
  planner.settings.axis_steps_per_mm[E_AXIS] = was;
}
#endif

MARLIN_TEST(gcode_reports, M203_reports_the_feedrate_limit_it_was_given) {
  const float was = planner.settings.max_feedrate_mm_s[X_AXIS];
  host_sends("M203 X250");
  TEST_ASSERT_TRUE(contains(reply_to("M203"), "M203 X250.00"));
  planner.settings.max_feedrate_mm_s[X_AXIS] = was;
}

MARLIN_TEST(gcode_reports, M201_reports_the_acceleration_limit_it_was_given) {
  const uint32_t was = planner.settings.max_acceleration_mm_per_s2[X_AXIS];
  host_sends("M201 X1250");
  TEST_ASSERT_TRUE(contains(reply_to("M201"), "M201 X1250.00"));
  planner.settings.max_acceleration_mm_per_s2[X_AXIS] = was;
}

MARLIN_TEST(gcode_reports, M204_reports_the_accelerations_it_was_given) {
  const float p = planner.settings.acceleration, r = planner.settings.retract_acceleration,
              t = planner.settings.travel_acceleration;
  host_sends("M204 P400 R900 T1900");
  const std::string reply = reply_to("M204");
  TEST_ASSERT_TRUE(contains(reply, "P400.00"));
  TEST_ASSERT_TRUE(contains(reply, "R900.00"));
  TEST_ASSERT_TRUE(contains(reply, "T1900.00"));
  planner.settings.acceleration = p;
  planner.settings.retract_acceleration = r;
  planner.settings.travel_acceleration = t;
}

MARLIN_TEST(gcode_reports, M206_reports_the_home_offset_it_was_given) {
  const float was = motion.home_offset.x;
  host_sends("M206 X7.5");
  TEST_ASSERT_TRUE(contains(reply_to("M206"), "M206 X7.50"));
  motion.set_home_offset(X_AXIS, was);
}

#if ENABLED(PREVENT_COLD_EXTRUSION)
  MARLIN_TEST(gcode_reports, M302_reports_whether_cold_extrusion_is_allowed) {
    const celsius_t was_temp = thermalManager.extrude_min_temp;
    const bool was_allowed = thermalManager.allow_cold_extrude;

    host_sends("M302 S170");
    const std::string disabled = reply_to("M302");
    TEST_ASSERT_TRUE(contains(disabled, "disabled"));
    TEST_ASSERT_TRUE(contains(disabled, "170"));

    host_sends("M302 P1");
    TEST_ASSERT_TRUE(contains(reply_to("M302"), "enabled"));

    thermalManager.extrude_min_temp = was_temp;
    thermalManager.allow_cold_extrude = was_allowed;
  }
#endif

// M105 is what a host polls constantly to draw the temperature graph.
MARLIN_TEST(gcode_reports, M105_reports_current_and_target_temperatures) {
  const std::string reply = reply_to("M105");
  TEST_ASSERT_TRUE(contains(reply, "T:"));      // hotend current
  TEST_ASSERT_TRUE(contains(reply, "/"));       // ...and its target
  TEST_ASSERT_TRUE(contains(reply, "B:"));      // bed
}

// M114 is what a host asks to show where the tool is.
MARLIN_TEST(gcode_reports, M114_reports_the_position) {
  const std::string reply = reply_to("M114");
  TEST_ASSERT_TRUE(contains(reply, "X:"));
  TEST_ASSERT_TRUE(contains(reply, "Y:"));
  TEST_ASSERT_TRUE(contains(reply, "Z:"));
  TEST_ASSERT_TRUE(contains(reply, "Count"));   // native step counts follow
}

// M115 is how a host discovers what this firmware can do.
MARLIN_TEST(gcode_reports, M115_reports_firmware_capabilities) {
  const std::string reply = reply_to("M115");
  TEST_ASSERT_TRUE(contains(reply, "FIRMWARE_NAME:"));
  TEST_ASSERT_TRUE(contains(reply, "Marlin"));
}
