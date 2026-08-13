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
 * The first tests of an LCD driver in this fork.
 *
 * A DWIN panel is not an interface the firmware calls; it is a screen the firmware writes
 * bytes at. So there is no equivalent of `stub_extui`'s recorded callbacks — the byte
 * stream on `LCD_SERIAL` is the only thing an outside observer can see, and a test that
 * cannot read it can only assert that the firmware did not crash while drawing.
 *
 * `SerialCapture` takes the port to watch, so the same drainer the host-facing tests use
 * works here. It has to drain from another thread: the write busy-waits for room in a
 * 128-byte buffer, and a screen refresh is longer than that, so draining afterwards would
 * be draining a buffer whose producer is already wedged.
 */

#include "../test/unit_tests.h"
#include "src/inc/MarlinConfig.h"

#if ENABLED(DWIN_CREALITY_LCD)

#include "src/lcd/dwin/creality/dwin.h"
#include "src/lcd/dwin/common/dwin_api.h"
#include "src/module/planner.h"
#include "src/lcd/marlinui.h"
#include "src/MarlinCore.h"
#include "../gcode/serial_capture.h"
#include "../support/simulated_encoder.h"

MARLIN_TEST(dwin_display, a_status_message_reaches_the_panel) {
  SerialCapture panel(LCD_SERIAL);
  dwinStatusChanged("RESCUED");
  const std::string sent = panel.finish();

  TEST_ASSERT_TRUE_MESSAGE(sent.size() > 0,
    "changing the status should put bytes on the display's serial port");
}

#if ENABLED(EDITABLE_STEPS_PER_UNIT)

/**
 * Register #33, confirmed by behaviour rather than by reading the source.
 *
 * `planner.settings.axis_steps_per_mm` has a derived cache, `planner.mm_per_step`, which is
 * its reciprocal — and keeping the two in step is the caller's job, done by calling
 * `refresh_positioning()`. This driver writes the array and does not. So the machine goes on
 * moving at a scale that no longer matches the one it reports, and nothing complains.
 *
 * The test says exactly that and no more: after a person changes the resolution at the panel,
 * the stored value changed and the reciprocal did not follow it. It does not assert the
 * arithmetic of the new value — the driver scales by a factor private to its own translation
 * unit, and predicting it here would be re-implementing the code under test.
 *
 * Nothing is set behind the driver's back. The starting value is deliberately below the
 * driver's own lower limit, so the first turn of the knob is clamped up to it — which makes
 * the committed value the driver's choice rather than the test's.
 */
MARLIN_TEST(dwin_display, changing_the_resolution_at_the_panel_leaves_the_reciprocal_stale) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;              // a click on a dark panel only wakes the screen
  marlin.wait_for_user = false;     // ... and a click that resumes a wait is not an ENTER

  const float steps_before = planner.settings.axis_steps_per_mm[X_AXIS],
              per_step_before = planner.mm_per_step[X_AXIS];

  // The panel, showing the X steps-per-millimetre editor.
  checkkey = ID_StepValue;
  hmiFlag.step_axis = X_AXIS;
  hmiValues.maxStepScaled = 0;

  const auto pump = [] { dwinHandleScreen(); };
  knob.turn_clockwise(pump);
  knob.click(pump);

  panel.finish();

  const float steps_after = planner.settings.axis_steps_per_mm[X_AXIS];

  TEST_ASSERT_NOT_EQUAL_MESSAGE(steps_before, steps_after,
    "turning the knob and clicking should store a new resolution for X");

  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(per_step_before, planner.mm_per_step[X_AXIS],
    "the reciprocal should have gone stale - it is not refreshed after the write (register #33)");

  TEST_ASSERT_FALSE_MESSAGE(NEAR(planner.mm_per_step[X_AXIS], 1.0f / steps_after),
    "and being stale means it no longer is the reciprocal of the stored resolution");

  // Put the machine back the way it was found, both halves together this time.
  planner.set_steps_per_mm(X_AXIS, steps_before);
}

#endif // EDITABLE_STEPS_PER_UNIT

#endif // ENABLED(DWIN_CREALITY_LCD)
