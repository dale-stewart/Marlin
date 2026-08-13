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
 * Tests for the endstops.
 *
 * Endstops are what stops an axis driving itself into the frame. The switches are
 * simulated pins in this build, so a test can press one by writing to the pin the
 * firmware reads — no production seam is needed.
 */

#include "../test/unit_tests.h"
#include "src/module/endstops.h"
#include "../gcode/serial_capture.h"
#include <string>
#include <stdio.h>

namespace {

  // What the report says about one switch: the word after its name.
  std::string state_of(const std::string &report, const char * const name) {
    const size_t at = report.find(name);
    if (at == std::string::npos) return "<absent>";
    const size_t colon = report.find(':', at);
    if (colon == std::string::npos) return "<no colon>";
    const size_t from = report.find_first_not_of(" ", colon + 1);
    const size_t to = report.find_first_of(" \r\n", from);
    return report.substr(from, to - from);
  }

  struct EndstopFixture {
    bool was_global;
    EndstopFixture() { was_global = endstops.global_enabled(); }
    ~EndstopFixture() {
      endstops.enable_globally(was_global);
      endstops.hit_on_purpose();
    }
  };

}

// The global switch is what M121 turns off to let a macro drive past a limit
// deliberately, and M120 turns back on.
MARLIN_TEST(endstops, the_global_switch_can_be_turned_off_and_on) {
  EndstopFixture fixture;

  endstops.enable_globally(false);
  TEST_ASSERT_FALSE(endstops.global_enabled());

  endstops.enable_globally(true);
  TEST_ASSERT_TRUE(endstops.global_enabled());
}

MARLIN_TEST(endstops, nothing_is_triggered_to_begin_with) {
  EndstopFixture fixture;
  endstops.hit_on_purpose();                 // clear any leftover flags
  TEST_ASSERT_FALSE(endstops.trigger_state());
}

/**
 * A printer that is not moving reports nothing, however its switches are set.
 *
 * `endstops.update()` only records a hit when the axis is moving *towards* the switch —
 * it reads the stepper's direction — so a pressed pin on a stationary machine changes
 * nothing. That is the behaviour, not a limitation of the harness: pressing a switch
 * on a *moving* axis is covered in `test_homing.cpp`, which runs under the test HAL.
 */
MARLIN_TEST(endstops, an_idle_printer_reports_nothing_triggered) {
  EndstopFixture fixture;
  endstops.hit_on_purpose();
  endstops.update();
  TEST_ASSERT_FALSE(endstops.trigger_state());
}

/**
 * Every switch is reported from its own pin.
 *
 * `report_states()` reads each endstop through one macro expanded per switch, so the whole
 * report is a single source line — and a fault in any one expansion is invisible to a test
 * that only ever presses one switch, because every other comparison agrees with an open
 * one whatever it says. Sixty-five mutants of that line survived a test that closed X.
 *
 * Each switch is therefore closed on its own, and what is asserted is that *its* entry
 * changed and its neighbours' did not. That is the same shape as
 * `each_axis_homes_against_its_own_switch`: a report built from a collection needs the
 * deciding element moved through the collection, not pressed once at the end.
 *
 * The pins are written directly here rather than driven through a simulated carriage. This
 * is a test of the reporting path — pin, inversion setting, name — and not of motion;
 * `M119_reports_each_switch_as_it_actually_is` drives one through the carriage to show the
 * two agree.
 */
MARLIN_TEST(endstops, each_switch_is_reported_from_its_own_pin) {
  EndstopFixture fixture;

  struct Switch { pin_t pin; uint8_t hit; const char *name; };
  const Switch switches[] = {
    { X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE, STR_X_MIN },
    { Y_MIN_PIN, Y_MIN_ENDSTOP_HIT_STATE, STR_Y_MIN },
    { Z_MIN_PIN, Z_MIN_ENDSTOP_HIT_STATE, STR_Z_MIN }
  };
  constexpr size_t COUNT = sizeof(switches) / sizeof(switches[0]);

  // Everything open, as the baseline every closed reading is compared against.
  for (size_t i = 0; i < COUNT; i++) WRITE(switches[i].pin, !switches[i].hit);
  {
    SerialCapture host; endstops.report_states();
    const std::string all_open = host.finish();
    for (size_t i = 0; i < COUNT; i++)
      TEST_ASSERT_EQUAL_STRING_MESSAGE(STR_ENDSTOP_OPEN,
        state_of(all_open, switches[i].name).c_str(),
        "with nothing pressed every switch should read open");
  }

  for (size_t closed = 0; closed < COUNT; closed++) {
    WRITE(switches[closed].pin, switches[closed].hit);
    std::string report;
    { SerialCapture host; endstops.report_states(); report = host.finish(); }
    WRITE(switches[closed].pin, !switches[closed].hit);

    for (size_t i = 0; i < COUNT; i++) {
      // Against the words the firmware defines, not against the other reading. Comparing
      // a closed report with an open one only says they differ, and a fault that inverts
      // every switch inverts both — the first version of this test did exactly that and
      // killed nothing.
      const char * const expected = (i == closed) ? STR_ENDSTOP_HIT : STR_ENDSTOP_OPEN;
      char msg[200];
      snprintf(msg, sizeof(msg), "with %s closed, %s should read %s",
               switches[closed].name, switches[i].name, expected);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, state_of(report, switches[i].name).c_str(), msg);
    }
  }
}

// The switches are named in axis order, which is what a person reading the report scans.
MARLIN_TEST(endstops, the_report_lists_the_switches_in_axis_order) {
  EndstopFixture fixture;
  SerialCapture host;

  endstops.report_states();
  const std::string report = host.finish();

  const size_t x = report.find(STR_X_MIN),
               y = report.find(STR_Y_MIN),
               z = report.find(STR_Z_MIN);

  TEST_ASSERT_TRUE_MESSAGE(x != std::string::npos && y != std::string::npos && z != std::string::npos,
    "all three minimum switches should be named");
  TEST_ASSERT_TRUE_MESSAGE(x < y, "X should be reported before Y");
  TEST_ASSERT_TRUE_MESSAGE(y < z, "and Y before Z");
}

/**
 * Turning endstop checking on waits for the switch readings to settle.
 *
 * `enable()` and `enable_globally()` both end in `resync()`, whose whole job is to not
 * return until the interrupt that samples the switches has run at least once. Skip it and
 * the very next move is planned against readings from before the change — which on a
 * machine that has just been told to start watching its limits is the one moment it
 * matters.
 *
 * There is nothing to observe but the clock. `resync()` returns void, changes no state a
 * caller can see, and its body is a delay; every mutant of it survived every assertion the
 * suite had. Timing it is not a stand-in for a better assertion, it *is* the assertion —
 * the third instance of that shape here, after `planner.synchronize()` and `M81`'s pause.
 *
 * Bracketed against the disabled case, which is the guard at the top: with nothing to watch
 * for, `resync()` returns at once rather than waiting. A version that always waited, or
 * never did, satisfies one of these two and not both.
 */
MARLIN_TEST(endstops, enabling_the_endstops_waits_for_the_readings_to_settle) {
  EndstopFixture fixture;

  endstops.enable_globally(false);
  endstops.enable(false);

  const millis_t before_off = millis();
  endstops.enable(false);                    // nothing to watch: no reason to wait
  const millis_t took_off = millis() - before_off;

  const millis_t before_on = millis();
  endstops.enable(true);                     // now watching: wait for a fresh sample
  const millis_t took_on = millis() - before_on;

  TEST_ASSERT_EQUAL_MESSAGE(0, took_off,
    "with endstop checking off there is nothing to resync, so nothing should wait");
  TEST_ASSERT_TRUE_MESSAGE(took_on >= 2,
    "turning it on should wait for the sampling interrupt to run at least once");
  TEST_ASSERT_TRUE_MESSAGE(took_on < 3,
    "and only for that, not for an interval of its own choosing");
}

// `enable_globally()` is the same story through the other entry point — M120/M121 rather
// than the motion code — and it has its own copy of the call.
MARLIN_TEST(endstops, enabling_them_globally_waits_the_same_way) {
  EndstopFixture fixture;

  endstops.enable_globally(false);
  endstops.enable(false);

  const millis_t before = millis();
  endstops.enable_globally(true);
  const millis_t took = millis() - before;

  TEST_ASSERT_TRUE_MESSAGE(took >= 2,
    "M120 should wait for a fresh sample too, not just set a flag");
  TEST_ASSERT_TRUE_MESSAGE(took < 5, "and no longer than that");
}
