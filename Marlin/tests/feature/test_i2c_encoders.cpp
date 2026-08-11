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
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "../support/simulated_i2c_encoder.h"
#include "src/module/planner.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

#include <string.h>
#include <string>

namespace {

  /**
   * The firmware's encoder object, pointed at an address and an axis.
   *
   * `I2CPositionEncodersMgr::init()` runs from `Marlin::setup()`, which a test build never
   * reaches, so encoder 0 starts with no address and would not talk to anything wherever a
   * device was attached. The fixture does what setup would have.
   *
   * It also *reads* the encoder before asking whether it passed. `passes_test()` reports the
   * field strength recorded by the last `get_raw_count()` and does not fetch one itself, so
   * asking without reading gets the "never seen" answer whatever is on the bus — which is how
   * the first version of the bad-field test below passed against a perfectly healthy encoder.
   */
  struct ConfiguredEncoder {
    ConfiguredEncoder(const AxisEnum axis = X_AXIS) {
      I2CPEM.encoders[0].init(I2CPE_ENC_1_ADDR, axis);
    }
    static bool passes_its_test() {
      I2CPEM.encoders[0].get_raw_count();          // refresh the field-strength reading
      return I2CPEM.encoders[0].passes_test(false);
    }
  };

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

/**
 * An encoder that answers is detected, and reports where the carriage is.
 *
 * The other arm of the two tests above, and the one that says they mean something: a suite where
 * nothing ever answers would be equally satisfied by a firmware that reported "not detected"
 * unconditionally.
 *
 * The reading is checked against the *carriage*, not against anything the firmware computed.
 * That is the whole point of closed-loop feedback — the encoder's job is to be able to disagree
 * with the machine's own idea of where it is, so a test that compared the two would be asserting
 * they agree by construction.
 */
MARLIN_TEST(i2c_encoders, an_encoder_that_answers_is_detected_and_reports_the_carriage) {
  SimulatedMachine machine;
  SimulatedAxisWithLimit rail(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                              X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
                              0, int32_t(0.0f * SimulatedMachine::STEPS_PER_MM));
  SimulatedI2CEncoder encoder(I2CPE_ENC_1_ADDR, rail,
                              SimulatedMachine::STEPS_PER_MM, I2CPE_ENC_1_TICKS_UNIT);

  ConfiguredEncoder configured;
  TEST_ASSERT_TRUE_MESSAGE(ConfiguredEncoder::passes_its_test(),
    "an encoder that answers with a good field strength should pass its test");

  // Drive the carriage 25 mm and ask the encoder where it thinks it is.
  encoder.zero_here();
  xyze_pos_t to = motion.position; to.x += 25.0f;
  TEST_ASSERT_TRUE(planner.buffer_line(to, 20.0f));
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the move never finished");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, 25.0f, encoder.mm(),
    "the encoder should have followed the carriage");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 25.0f * I2CPE_ENC_1_TICKS_UNIT,
    float(I2CPEM.encoders[0].get_raw_count()),
    "and the firmware should read that as the encoder's own count");
}

/**
 * A strip the head has drifted away from is reported, not read.
 *
 * A magnetic encoder that has lost its strip still answers on the bus — it simply answers with a
 * field strength saying so. Treating that as a position is worse than having no encoder at all,
 * because the firmware would correct towards a number that means nothing.
 */
MARLIN_TEST(i2c_encoders, an_encoder_that_has_lost_its_strip_fails_its_test) {
  SimulatedMachine machine;
  SimulatedAxisWithLimit rail(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                              X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE, 0, 0);
  SimulatedI2CEncoder encoder(I2CPE_ENC_1_ADDR, rail,
                              SimulatedMachine::STEPS_PER_MM, I2CPE_ENC_1_TICKS_UNIT);

  ConfiguredEncoder configured;
  TEST_ASSERT_TRUE_MESSAGE(ConfiguredEncoder::passes_its_test(),
    "this test needs a healthy encoder to begin with, or it proves nothing");

  encoder.report_field_strength(2);   // I2CPE_MAG_SIG_BAD

  TEST_ASSERT_FALSE_MESSAGE(ConfiguredEncoder::passes_its_test(),
    "an encoder reporting a bad field should fail its test");
}

#endif // __PLAT_TEST__ && I2C_POSITION_ENCODERS
