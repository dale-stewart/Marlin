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
 * The emergency stop.
 *
 * `kill()` is what the firmware calls when it has decided the machine must not carry on: a
 * thermal runaway, a sensor it cannot believe, `M112` from the host, a failed heat-up during
 * autotune. Everything it does is a last act, so there is no second chance at any of it — a
 * heater left on here is a heater left on for good.
 *
 * This was recorded as untestable for a long time, and the record was wrong. See
 * `support/kill_button.h` for the whole story; the short version is that `minkill()` waits for
 * the kill button to be **released and then pressed** before rebooting, `MarlinHAL::reboot()`
 * is empty under the test HAL, and nobody had ever pressed the button. It hangs waiting for an
 * operator, which is what a halted printer is supposed to do.
 *
 * What that opened, and what these tests are therefore about: the order of the shutdown, the
 * message the host is left with, and the difference between the two kinds of kill.
 */

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/kill_button.h"
#include "../gcode/simulated_sensors.h"
#include "../gcode/serial_capture.h"
#include "src/MarlinCore.h"
#include "src/module/temperature.h"
#include "src/module/stepper.h"
#include "src/module/planner.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include <string.h>

#if HAS_KILL

namespace {

  struct SavedMachineState {
    MarlinState was_state;
    celsius_t hotend;
    #if HAS_HEATED_BED
      celsius_t bed;
    #endif
    SavedMachineState() {
      was_state = marlin.state;
      hotend = thermalManager.degTargetHotend(0);
      TERN_(HAS_HEATED_BED, bed = thermalManager.degTargetBed());
    }
    ~SavedMachineState() {
      thermalManager.setTargetHotend(hotend, 0);
      TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(bed));
      marlin.setState(was_state);
      SimulatedHardware::release_kill_button();
    }
  };

  // Everything switched on, so that "it went off" is a statement about `kill()` rather than
  // about a machine that was already idle.
  void a_machine_that_is_running() {
    SimulatedSensors::hotend_reads(25.0f);
    thermalManager.setTargetHotend(200, 0);
    #if HAS_HEATED_BED
      SimulatedSensors::bed_reads(25.0f);
      thermalManager.setTargetBed(60);
    #endif
    stepper.enable_all_steppers();
    for (uint32_t i = 0; i < 200; i++) { HAL_test_advance_millis(1); thermalManager.task(); }
  }

  bool hotend_heater_is_powered() {
    return bool(READ(HEATER_0_PIN)) != bool(ENABLED(HEATER_0_INVERTING));
  }

  bool an_axis_is_energised() {
    return bool(READ(X_ENABLE_PIN)) == bool(ENABLED(X_ENABLE_ON));
  }

  #if HAS_EXTRUDERS
    bool the_extruder_is_energised() {
      return bool(READ(E0_ENABLE_PIN)) == bool(ENABLED(E_ENABLE_ON));
    }
  #endif

}

// ---------------------------------------------------------------------------
// What a kill switches off
// ---------------------------------------------------------------------------

/**
 * Every heater goes off, and that is the first thing that happens.
 *
 * The reason `kill()` disables the heaters before it prints anything, before it draws a screen
 * and before it waits for anyone is that all of those can fail or block. A heater left driving
 * while the firmware is busy telling somebody about it is the failure this function exists to
 * prevent.
 */
MARLIN_TEST(kill, every_heater_goes_off) {
  SimulatedMachine machine;
  SavedMachineState saved;
  SimulatedSensors sensors;

  a_machine_that_is_running();
  TEST_ASSERT_TRUE_MESSAGE(thermalManager.temp_hotend[0].soft_pwm_amount > 0,
    "the fixture should have the hotend heating before the machine is killed");

  OperatorPressesKill operator_arrives;
  marlin.kill();

  TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetHotend(0),
    "a killed machine should have no hotend target left");
  TEST_ASSERT_FALSE_MESSAGE(hotend_heater_is_powered(), "and no power going to the heater");
  #if HAS_HEATED_BED
    TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetBed(),
      "and no bed target either");
  #endif
}

/**
 * The two kinds of kill differ in what they do to the motors, and the difference is deliberate.
 *
 * A plain `kill()` releases only the extruder: the machine has stopped, but the axes stay
 * energised so a gantry with nothing holding it up does not drop onto the print. `M112` passes
 * `steppers_off`, because an emergency stop means *everything* off and a person is about to put
 * their hands in the machine.
 *
 * Asserting one without the other would pass against firmware that always did the same thing,
 * which is the whole point of the flag.
 */
#if HAS_EXTRUDERS
  MARLIN_TEST(kill, a_plain_kill_releases_the_extruder_and_holds_the_axes) {
    SimulatedMachine machine;
    SavedMachineState saved;
    SimulatedSensors sensors;

    a_machine_that_is_running();
    TEST_ASSERT_TRUE_MESSAGE(an_axis_is_energised(), "the fixture should have the axes energised");
    TEST_ASSERT_TRUE_MESSAGE(the_extruder_is_energised(), "and the extruder too");

    OperatorPressesKill operator_arrives;
    marlin.kill(nullptr, nullptr, false);

    TEST_ASSERT_FALSE_MESSAGE(the_extruder_is_energised(),
      "a kill should release the extruder");
    TEST_ASSERT_TRUE_MESSAGE(an_axis_is_energised(),
      "but hold the axes, so nothing drops onto the print");
  }

  MARLIN_TEST(kill, an_emergency_stop_releases_everything) {
    SimulatedMachine machine;
    SavedMachineState saved;
    SimulatedSensors sensors;

    a_machine_that_is_running();
    TEST_ASSERT_TRUE(an_axis_is_energised());

    OperatorPressesKill operator_arrives;
    marlin.kill(nullptr, nullptr, true);

    TEST_ASSERT_FALSE_MESSAGE(an_axis_is_energised(),
      "an emergency stop should release the axes as well");
    TEST_ASSERT_FALSE_MESSAGE(the_extruder_is_energised(), "and the extruder");
  }
#endif

// ---------------------------------------------------------------------------
// What it says
// ---------------------------------------------------------------------------

/**
 * The host is told the printer is halted, and told why.
 *
 * Two separate messages with two separate jobs. `Error:Printer halted. kill() called!` is the
 * fixed one a host watches for to know the connection is now useless; the reason is whatever
 * the caller passed, and it is the only record of *which* fault stopped the machine. A kill
 * that printed one without the other would leave either an unexplained halt or an explanation
 * nobody was watching for.
 */
MARLIN_TEST(kill, the_host_is_told_the_printer_is_halted_and_why) {
  SimulatedMachine machine;
  SavedMachineState saved;
  SimulatedSensors sensors;

  std::string reply;
  {
    SerialCapture capture;
    OperatorPressesKill operator_arrives;
    marlin.kill(F("Thermal Runaway"), F("E1"));
    reply = capture.finish();
  }

  TEST_ASSERT_TRUE_MESSAGE(reply.find("Error:" STR_ERR_KILLED) != std::string::npos,
    "a killed machine should tell the host it is halted, on the error channel");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("Thermal Runaway") != std::string::npos,
    "and should say what stopped it");
}

/**
 * A kill with no reason still says it is halted.
 *
 * `kill()` is called from places that have nothing useful to add — `M112` is the operator
 * themselves — and the message that matters is the fixed one. The `if (lcd_error)` guard is
 * what makes the reason optional, and a machine that skipped the whole report when there was
 * no reason to give would go silent exactly when a person had pressed the panic button.
 */
MARLIN_TEST(kill, a_kill_with_no_reason_still_reports_the_halt) {
  SimulatedMachine machine;
  SavedMachineState saved;
  SimulatedSensors sensors;

  std::string reply;
  {
    SerialCapture capture;
    OperatorPressesKill operator_arrives;
    marlin.kill();
    reply = capture.finish();
  }

  TEST_ASSERT_TRUE_MESSAGE(reply.find("Error:" STR_ERR_KILLED) != std::string::npos,
    "a kill with no reason should still report the halt");
}

// ---------------------------------------------------------------------------
// What it waits for
// ---------------------------------------------------------------------------

/**
 * The button has to be released before a press counts.
 *
 * `minkill()` waits for release and *then* for a press, which is what stops the machine
 * rebooting the instant it is killed by an operator who is still holding the button down —
 * they would never see the error, and the fault that caused it would be lost.
 *
 * The machine here is killed with the button already held, and the assertion is that the
 * operator had let go before `kill()` returned. That is deliberately a flag rather than a
 * clock reading: `kill()` returns while the button is still *down* — it stops at the press,
 * not at the following release — so the pin state afterwards says nothing, and a wall-clock
 * bound would be a timing race. Firmware that accepted the held button would return before
 * the release ever happened, and the flag would still be false.
 */
MARLIN_TEST(kill, a_button_already_held_is_not_the_press_it_waits_for) {
  SimulatedMachine machine;
  SavedMachineState saved;
  SimulatedSensors sensors;

  OperatorPressesKill operator_arrives = OperatorPressesKill::already_holding_it();
  TEST_ASSERT_TRUE_MESSAGE(marlin.kill_state(),
    "this test is about a machine killed while the button is down");

  marlin.kill();

  TEST_ASSERT_TRUE_MESSAGE(OperatorPressesKill::the_operator_let_go_first(),
    "the machine should have waited for the release before accepting a press");
}

#endif // HAS_KILL
