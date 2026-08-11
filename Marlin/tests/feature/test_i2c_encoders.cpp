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
 * A machine that was told it has position encoders, and has not.
 *
 * Closed-loop feedback is only worth having if the firmware knows when it is not getting any.
 * An encoder that is unplugged, mis-addressed, or too far from its magnetic strip reads as
 * nothing at all — and the failure a user cares about is the quiet one, where the machine
 * carries on believing it has feedback it is not receiving.
 *
 * `HAL/TEST/include/Wire.h` models an idle bus: nothing acknowledges and no bytes come back,
 * which is exactly what a board sees with an empty header. So this is the disconnected case,
 * and it is the case a test can reach without simulating a device.
 */

#include "src/inc/MarlinConfig.h"

#if defined(__PLAT_TEST__) && ENABLED(I2C_POSITION_ENCODERS)

#include "../test/unit_tests.h"
#include "../gcode/serial_capture.h"
#include "src/feature/encoder_i2c.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

#include <string.h>
#include <string>

namespace {

  std::string host_sends(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    SerialCapture capture;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    return capture.finish();
  }

}

/**
 * An encoder that does not answer is reported as not detected.
 *
 * `M861` asks each module how it is doing. With nothing on the bus the answer has to be that
 * the encoder is not there — said out loud, because the alternative is a machine that reports
 * a position of zero and looks like it is working.
 */
MARLIN_TEST(i2c_encoders, an_encoder_that_does_not_answer_is_reported_as_missing) {
  const std::string said = host_sends("M861");

  TEST_ASSERT_TRUE_MESSAGE(said.find("not detected") != std::string::npos,
    "a status report with nothing on the bus should say the encoder was not detected");
}

/**
 * ...and it is not treated as an active encoder.
 *
 * The report is the visible half; this is the half that matters. A module that failed its test
 * must not be left active, or the firmware will apply corrections computed from readings it
 * never received.
 */
MARLIN_TEST(i2c_encoders, an_encoder_that_does_not_answer_is_not_left_active) {
  host_sends("M861");

  TEST_ASSERT_FALSE_MESSAGE(I2CPEM.encoders[0].get_active(),
    "an encoder that failed its test should not be left active");
}

#endif // __PLAT_TEST__ && I2C_POSITION_ENCODERS
