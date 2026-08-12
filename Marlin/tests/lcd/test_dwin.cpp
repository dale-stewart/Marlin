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

#if defined(__PLAT_TEST__) && ENABLED(DWIN_CREALITY_LCD)

#include "src/lcd/dwin/creality/dwin.h"
#include "src/lcd/dwin/common/dwin_api.h"
#include "src/module/planner.h"
#include "src/MarlinCore.h"
#include "../gcode/serial_capture.h"

MARLIN_TEST(dwin_display, a_status_message_reaches_the_panel) {
  SerialCapture panel(LCD_SERIAL);
  dwinStatusChanged("RESCUED");
  const std::string sent = panel.finish();

  TEST_ASSERT_TRUE_MESSAGE(sent.size() > 0,
    "changing the status should put bytes on the display's serial port");
}

/**
 * Register #33 cannot be driven from here, and the reason is worth more than the test.
 *
 * The write that leaves the reciprocal stale lives in `hmiStepXYZE()`, and it only happens
 * on an encoder click: `encoderReceiveAnalyze()` gates it on `BUTTON_PRESSED(ENC)`, which is
 * a read of `BTN_ENC`. `BOARD_SIMULATED` defines no encoder pins, so that macro is a
 * compile-time false and no sequence of calls from a test reaches the assignment.
 *
 * So making a driver host-buildable is not the same as making it drivable. What is
 * observable here is everything the firmware *sends* — that is what the test above uses —
 * and what is not is anything a person would have done to the machine by hand. Confirming
 * #33 by behaviour needs one of: a board definition with encoder pins, or a simulated
 * encoder attached the way `SimulatedI2CEncoder` attaches to the I2C bus.
 *
 * Recorded rather than faked. Setting `planner.settings.axis_steps_per_mm` from the test and
 * asserting the reciprocal went stale would pass, prove nothing about the driver, and read
 * for all the world like a driver test.
 */

#endif // __PLAT_TEST__ && DWIN_CREALITY_LCD
