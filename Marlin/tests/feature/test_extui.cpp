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
 * What the firmware tells the display.
 *
 * `EXTENSIBLE_UI` is the seam every third-party screen hangs off: the firmware calls
 * `ExtUI::on*` when something happens and the display decides what to draw. The failure that
 * matters is silence — a homing move that never announces itself, a reset the screen never hears
 * about — because a display showing stale information is worse than one showing nothing, and
 * nothing in the firmware notices.
 *
 * These go through G-code and assert on what a display would have received, using the recording
 * client in `support/stub_extui.h`. Nothing here calls `ui_api.cpp` directly: the point is that
 * the *firmware* reaches it.
 */

#include "src/inc/MarlinConfig.h"

#if ENABLED(EXTENSIBLE_UI)

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "../support/stub_extui.h"
#include "../gcode/serial_capture.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/lcd/extui/ui_api.h"

#include <string.h>

namespace {

  void host_sends(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    SerialCapture capture;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    (void)capture.finish();
  }

  // The recorder is static and outlives whatever filled it.
  struct WatchingDisplay {
    WatchingDisplay() { RecordedUI::reset(); }
    ~WatchingDisplay() { RecordedUI::reset(); }
  };

}

/**
 * Homing tells the display when it starts and when it is done.
 *
 * A screen that missed the start would show the machine idle while it is moving; one that missed
 * the end would show it homing forever. Both halves, because a firmware that announced only the
 * start satisfies half of this and leaves the display stuck.
 */
MARLIN_TEST(extui, homing_tells_the_display_it_started_and_finished) {
  SimulatedMachine machine;
  WatchingDisplay display;
  SimulatedAxisWithLimit x(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                           X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE, 0, int32_t(20.0f * SimulatedMachine::STEPS_PER_MM));

  motion.set_axis_never_homed(X_AXIS);
  host_sends("G28 X");
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  TEST_ASSERT_TRUE_MESSAGE(RecordedUI::homing_starts > 0, "the display should have been told homing started");
  TEST_ASSERT_TRUE_MESSAGE(RecordedUI::homing_dones > 0, "and that it finished");
}

/**
 * Restoring the factory settings tells the display.
 *
 * Every value a screen shows about the machine's configuration has just changed underneath it,
 * so this is the notification that says "redraw everything". A display that did not hear it
 * would keep showing the settings the user has just discarded.
 */
MARLIN_TEST(extui, a_factory_reset_tells_the_display) {
  SimulatedMachine machine;
  WatchingDisplay display;

  host_sends("M502");

  TEST_ASSERT_TRUE_MESSAGE(RecordedUI::factory_resets > 0,
    "the display should have been told the settings went back to their defaults");
}

/**
 * A status message reaches the display, with its text.
 *
 * The status line is the one part of a screen the firmware writes directly, and it is how a
 * machine says what it is doing. Asserting the *text* rather than that something arrived: a
 * notification carrying the wrong message is as bad as none, and only checking the count would
 * not see it.
 */
MARLIN_TEST(extui, a_status_message_reaches_the_display) {
  SimulatedMachine machine;
  WatchingDisplay display;

  host_sends("M117 Levelling the bed");

  TEST_ASSERT_TRUE_MESSAGE(RecordedUI::last_status.find("Levelling the bed") != std::string::npos,
    "the display should have been given the message the host set");
}

//
// ---- The other direction: what a display asks the firmware to do ----
//
// Everything above is the firmware telling the display. `ui_api.cpp` is the reverse — the
// functions a screen calls to read and change the machine — and nothing exercises it unless a
// test stands in for the display, which is what these do.
//

/**
 * A display setting the axis resolution changes it, and keeps the reciprocal with it.
 *
 * This is the same invariant `M92` has to honour, reached from the other side of the firmware.
 * A screen that changed the resolution and left `mm_per_step` behind would leave the machine
 * converting with the old figure — the defect confirmed in `calibrate_steps_mm()` (register #35),
 * which is what makes it worth asserting here rather than assuming.
 */
MARLIN_TEST(extui, a_display_setting_the_resolution_keeps_the_reciprocal_with_it) {
  SimulatedMachine machine;
  WatchingDisplay display;

  ExtUI::setAxisSteps_per_mm(123.25f, ExtUI::X);

  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(123.25f, ExtUI::getAxisSteps_per_mm(ExtUI::X),
    "the display should read back the resolution it set");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(123.25f, planner.steps_per_mm(X_AXIS),
    "and the planner should be using it");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 1.0f / 123.25f, planner.mm_per_step[X_AXIS],
    "with the reciprocal it converts by kept in step");
}

/**
 * ...and each axis separately.
 *
 * The resolutions are a table and a display addresses them by index, so a setter that wrote one
 * entry for every axis, or the wrong one, would satisfy a single-axis check.
 */
MARLIN_TEST(extui, a_display_sets_the_axis_it_names) {
  SimulatedMachine machine;
  WatchingDisplay display;

  const float was_y = ExtUI::getAxisSteps_per_mm(ExtUI::Y);

  ExtUI::setAxisSteps_per_mm(77.5f, ExtUI::X);

  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(77.5f, ExtUI::getAxisSteps_per_mm(ExtUI::X), "X should have changed");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(was_y, ExtUI::getAxisSteps_per_mm(ExtUI::Y), "Y should not have");
}

#endif // ENABLED(EXTENSIBLE_UI)
