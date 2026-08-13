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
 * Commands that answer a question, and the one that shuts the machine down.
 *
 * `M119` says which switches are closed, which is what somebody troubleshooting a machine
 * that will not home types first — and its answer has to reflect the *switches*, not the
 * firmware's belief about them. `M155 S<n>` asks for temperatures on a timer instead of on
 * request. `M81` turns everything off.
 *
 * These sat at 0% together and none of them is trivial: `M119` reads pins through the same
 * inversion logic homing uses, `M155` clamps a period a host can set to anything, and `M81`
 * is a sequence of shutdown steps whose order is the safety property.
 */

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "../support/test_clock.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/endstops.h"
#include "src/module/temperature.h"
#include "src/module/planner.h"
#include "src/module/stepper.h"
#include "src/module/printcounter.h"
#include "src/MarlinCore.h"
#include "serial_capture.h"
#include <string.h>
#include <stdio.h>

namespace {

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  // What the report says about one switch: the word after its name.
  std::string state_of(const std::string &report, const char * const name) {
    const size_t at = report.find(name);
    if (at == std::string::npos) return "<absent>";
    const size_t colon = report.find(':', at);
    if (colon == std::string::npos) return "<no colon>";
    size_t from = report.find_first_not_of(" ", colon + 1);
    const size_t to = report.find_first_of(" \r\n", from);
    return report.substr(from, to - from);
  }

}

/**
 * `M119` reports what the switches are doing now, not what the firmware remembers.
 *
 * Bracketed, because that is the only way to say it. A report that always said "open"
 * satisfies any test that only closes nothing, and one that always said "TRIGGERED"
 * satisfies any test that only closes something. The same switch is read both ways round,
 * and the report has to change with it.
 *
 * Driven by moving the simulated carriage onto its limit rather than by writing the pin, so
 * what is asserted is the whole path a person would exercise with their finger — pin state,
 * the build's inversion setting, and the naming.
 */
MARLIN_TEST(reporting_commands, M119_reports_each_switch_as_it_actually_is) {
  SimulatedMachine machine;
  constexpr float SPM = SimulatedMachine::STEPS_PER_MM;

  std::string open_report, closed_report;
  {
    // Switch at 0 mm, carriage parked well clear of it.
    SimulatedAxisWithLimit x_rail(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                                  X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
                                  0, int32_t(20.0f * SPM));
    SerialCapture host;
    host_sends("M119");
    open_report = host.finish();
  }
  {
    // The same rail with the carriage sitting on the switch.
    SimulatedAxisWithLimit x_rail(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                                  X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
                                  int32_t(20.0f * SPM), 0);
    SerialCapture host;
    host_sends("M119");
    closed_report = host.finish();
  }

  TEST_ASSERT_TRUE_MESSAGE(open_report.find(STR_M119_REPORT) != std::string::npos,
    "M119 should announce itself as an endstop report");

  const std::string when_open = state_of(open_report, STR_X_MIN),
                    when_closed = state_of(closed_report, STR_X_MIN);

  TEST_ASSERT_TRUE_MESSAGE(when_open != "<absent>",
    "the report should name the X minimum switch this build has");
  TEST_ASSERT_TRUE_MESSAGE(when_open != when_closed,
    "and say something different about it when it is closed than when it is open");
}

// Every switch the build has gets a line, not just the one being pressed.
MARLIN_TEST(reporting_commands, M119_reports_every_switch_the_machine_has) {
  SimulatedMachine machine;
  SerialCapture host;

  host_sends("M119");
  const std::string report = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(state_of(report, STR_X_MIN) != "<absent>", "X min should be reported");
  TEST_ASSERT_TRUE_MESSAGE(state_of(report, STR_Y_MIN) != "<absent>", "and Y min");
  TEST_ASSERT_TRUE_MESSAGE(state_of(report, STR_Z_MIN) != "<absent>", "and Z min");
}

#if ENABLED(AUTO_REPORT_TEMPERATURES)

/**
 * `M155 S<n>` turns the temperature report from a question into a subscription.
 *
 * Asserted by letting the time pass and counting what arrives, not by reading back the
 * stored interval: the field is private machinery and the behaviour a host depends on is
 * that reports keep coming without being asked. Bracketed against `S0`, which is how a host
 * unsubscribes — and a version that ignored the period entirely would satisfy either test
 * alone.
 */
MARLIN_TEST(reporting_commands, M155_sends_temperatures_on_a_timer_until_told_to_stop) {
  SimulatedMachine machine;
  SerialCapture host;

  host_sends("M155 S1");
  for (uint8_t s = 0; s < 3; s++) { TestClock::advance_seconds(1); thermalManager.auto_reporter.tick(); }
  const std::string subscribed = host.finish();

  size_t reports = 0, from = 0;
  while ((from = subscribed.find("T:", from)) != std::string::npos) { reports++; from++; }
  TEST_ASSERT_TRUE_MESSAGE(reports >= 2,
    "M155 S1 should keep sending temperatures without being asked again");

  SerialCapture after;
  host_sends("M155 S0");
  for (uint8_t s = 0; s < 3; s++) { TestClock::advance_seconds(1); thermalManager.auto_reporter.tick(); }
  const std::string unsubscribed = after.finish();

  TEST_ASSERT_TRUE_MESSAGE(unsubscribed.find("T:") == std::string::npos,
    "and M155 S0 should stop them, which is how a host unsubscribes");
}

/**
 * LEGACY-BEHAVIOR (register #44): the clamp on the reporting period is only half applied.
 *
 * `set_interval()` limits the *repeat* interval to 60 seconds — `report_interval =
 * _MIN(seconds, limit)` — and then schedules the first report with the number the host
 * asked for: `next_report_ms = millis() + SEC_TO_MS(seconds)`. So `M155 S255` is clamped
 * for every report after the first and not for the first, and the machine goes silent for
 * four and a quarter minutes before the limit takes any effect at all.
 *
 * That is the exact silence the clamp exists to prevent, and to a host it is
 * indistinguishable from a printer that has stopped answering.
 *
 * Recorded rather than fixed. The one-word correction is to schedule from
 * `report_interval` instead of `seconds`, which changes what a machine does and is the
 * maintainer's call. Pinned from both sides so the current behaviour cannot drift
 * unnoticed and so the fix starts from a failing test.
 */
MARLIN_TEST(reporting_commands, a_period_beyond_the_limit_delays_the_first_report_anyway) {
  SimulatedMachine machine;

  host_sends("M155 S255");

  {
    SerialCapture host;
    TestClock::advance_seconds(61);           // past the limit the clamp claims to impose
    thermalManager.auto_reporter.tick();
    TEST_ASSERT_TRUE_MESSAGE(host.finish().find("T:") == std::string::npos,
      "LEGACY: nothing arrives inside the clamped period, because the first deadline was "
      "scheduled from the unclamped value");
  }
  {
    SerialCapture host;
    TestClock::advance_seconds(200);          // now past the period actually asked for
    thermalManager.auto_reporter.tick();
    TEST_ASSERT_TRUE_MESSAGE(host.finish().find("T:") != std::string::npos,
      "LEGACY: and the first report arrives after the full unclamped period instead");
  }

  host_sends("M155 S0");
}

// The clamp does work from the second report onwards, which is what makes the first one an
// oversight rather than a design. Asserted so that a fix cannot quietly break this half.
MARLIN_TEST(reporting_commands, the_repeat_period_is_clamped_even_when_the_first_one_is_not) {
  SimulatedMachine machine;

  host_sends("M155 S255");
  TestClock::advance_seconds(256);
  thermalManager.auto_reporter.tick();        // the late first report

  SerialCapture host;
  TestClock::advance_seconds(61);
  thermalManager.auto_reporter.tick();
  TEST_ASSERT_TRUE_MESSAGE(host.finish().find("T:") != std::string::npos,
    "once it has started, the period really is clamped to the limit");

  host_sends("M155 S0");
}

#endif // AUTO_REPORT_TEMPERATURES

/**
 * `M81` is the shutdown, and what matters is what it leaves behind.
 *
 * Three things, each of which is somebody's bad afternoon if it is missed: the motors are
 * released, every heater is told to cool, and the print timer stops so the job is not
 * recorded as still running. Asserted together because the command is one action from the
 * user's point of view, and separately from each other because a version that did one and
 * not the others would look like it worked.
 */
MARLIN_TEST(reporting_commands, M81_releases_the_motors_and_cools_everything_down) {
  SimulatedMachine machine;
  SerialCapture host;

  host_sends("M17");
  TERN_(HAS_HOTEND, thermalManager.setTargetHotend(200, 0));
  TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(60));
  print_job_timer.start();

  host_sends("M81");
  host.finish();

  TEST_ASSERT_FALSE_MESSAGE(TEST(stepper.axis_enabled.bits, X_AXIS),
    "M81 should release the motors");
  #if HAS_HOTEND
    TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetHotend(0),
      "and tell the hotend to cool");
  #endif
  #if HAS_HEATED_BED
    TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetBed(),
      "and the bed");
  #endif
  TEST_ASSERT_FALSE_MESSAGE(print_job_timer.isRunning(),
    "and stop the job timer, so a machine switched off mid-print is not still counting");
}

/**
 * And it waits a second before cutting the power.
 *
 * The pause is there so the last things `M81` did — the heaters told to cool, the message
 * put on the screen — are not immediately followed by the supply being switched off. It is
 * a bare `safe_delay(1000)` with nothing to observe but the clock, which is why every
 * mutant of it survived: no assertion about what `M81` *changed* can see how long it took.
 *
 * Bounded both ways. A lower bound alone is satisfied by a delay of any length, and the
 * upper one is loose because `idle()` advances simulated time in fixed steps and overshoots
 * slightly - a property of the harness rather than of the firmware.
 */
MARLIN_TEST(reporting_commands, M81_pauses_before_it_cuts_the_power) {
  SimulatedMachine machine;
  SerialCapture host;

  const millis_t before = millis();
  host_sends("M81");
  const millis_t took = millis() - before;
  host.finish();

  TEST_ASSERT_TRUE_MESSAGE(took >= 1000,
    "M81 should give the machine a second before switching off");
  TEST_ASSERT_TRUE_MESSAGE(took < 1500,
    "and not appreciably longer, which would look like a hung shutdown");
}
