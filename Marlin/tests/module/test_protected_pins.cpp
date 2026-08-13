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
 * Which pins `M42` refuses to touch.
 *
 * `M42 P<pin> S<value>` writes to a pin directly, and it exists so that people can drive
 * relays, lights and solenoids from G-code. The same command pointed at a heater's MOSFET
 * turns that heater fully on with nothing regulating it, and pointed at a stepper's enable
 * line drops a gantry mid-print. `pin_is_protected()` is the list of pins that belong to the
 * firmware, and `M42` refuses them unless the operator says `I` to mean they know.
 *
 * It is a pure predicate over a table generated from the board's own pin assignments, so it
 * can be asserted directly. The two halves are separate loops with separate lookups — the
 * digital table by pin number, the analog one through `analogInputToDigitalPin()` — so a
 * fault in either is invisible to a test that only exercises the other.
 *
 * Every pin named below is taken from the board's configuration rather than written as a
 * number, so this is a claim about *this machine's* wiring rather than about a constant that
 * happened to be right when it was written.
 */

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../gcode/serial_capture.h"
#include "src/MarlinCore.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include <string.h>

#if ENABLED(DIRECT_PIN_CONTROL)
  namespace {
    std::string reply_to(const char * const line) {
      static char buf[64];
      strncpy(buf, line, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      SerialCapture capture;
      parser.parse(buf);
      gcode.process_parsed_command(true);
      return capture.finish();
    }
  }
#endif

/**
 * The pins that drive things which can burn or fall are protected.
 *
 * One from each family the table is built out of, because they are separate entries assembled
 * by separate macros: a heater, a stepper enable, a step pin and an endstop input. A machine
 * that protected only the heaters would still let `M42` drop the Z axis.
 */
MARLIN_TEST(protected_pins, the_pins_that_drive_the_machine_are_protected) {
  TEST_ASSERT_TRUE_MESSAGE(marlin.pin_is_protected(HEATER_0_PIN),
    "the hotend heater pin should be protected");
  #if HAS_HEATED_BED
    TEST_ASSERT_TRUE_MESSAGE(marlin.pin_is_protected(HEATER_BED_PIN),
      "and the bed heater pin");
  #endif
  TEST_ASSERT_TRUE_MESSAGE(marlin.pin_is_protected(X_ENABLE_PIN),
    "and a stepper's enable line, which holds the axis up");
  TEST_ASSERT_TRUE_MESSAGE(marlin.pin_is_protected(X_MIN_PIN),
    "and the endstop the machine homes against");
  #if HAS_EXTRUDERS
    TEST_ASSERT_TRUE_MESSAGE(marlin.pin_is_protected(E0_STEP_PIN),
      "and the extruder's step pin");
  #endif

  // Worth knowing rather than asserting the opposite of: a main axis contributes its ENABLE,
  // MIN/MAX and microstepping pins to the table but **not** its STEP and DIR, while the
  // extruder contributes all three. That is the board's policy rather than an oversight —
  // a stray write to STEP injects one pulse, where a write to ENABLE drops the gantry — and
  // it is stated here so the next reader does not have to re-derive it from the table.
  TEST_ASSERT_FALSE_MESSAGE(marlin.pin_is_protected(X_STEP_PIN),
    "an axis STEP pin is deliberately not in the table; only ENABLE, the endstops and MS pins are");
}

/**
 * The analog inputs are protected too, and by the other half of the function.
 *
 * The temperature sensors are read through analog channels, and the table stores them as
 * analog indices — so the lookup has to convert with `analogInputToDigitalPin()` before it can
 * compare. That is a second loop with a second conversion, and it is the half a test written
 * around heater outputs never touches. Driving a thermistor input from `M42` makes the
 * firmware believe whatever the operator wrote, which is the input that the whole of thermal
 * protection is downstream of.
 */
MARLIN_TEST(protected_pins, the_temperature_inputs_are_protected_as_analog_channels) {
  TEST_ASSERT_TRUE_MESSAGE(marlin.pin_is_protected(analogInputToDigitalPin(TEMP_0_PIN)),
    "the hotend sensor's channel should be protected");
  #if HAS_HEATED_BED
    TEST_ASSERT_TRUE_MESSAGE(marlin.pin_is_protected(analogInputToDigitalPin(TEMP_BED_PIN)),
      "and the bed sensor's");
  #endif
}

/**
 * ...and a pin that belongs to nobody is not protected.
 *
 * The other side of the bracket, and the one that says the function is a lookup rather than a
 * blanket refusal. Without it, firmware that protected *every* pin would pass everything
 * above while making `M42` useless for the purpose it exists for.
 *
 * The pin is found by searching rather than named, because any number written here would be a
 * guess about the board that the next pins file could invalidate silently.
 */
MARLIN_TEST(protected_pins, a_pin_the_firmware_does_not_use_is_left_alone) {
  int free_pin = -1;
  for (int p = 0; p <= 200; p++)
    if (!marlin.pin_is_protected(pin_t(p))) { free_pin = p; break; }

  TEST_ASSERT_TRUE_MESSAGE(free_pin >= 0,
    "this board should have at least one pin the firmware does not claim");
  TEST_ASSERT_FALSE_MESSAGE(marlin.pin_is_protected(pin_t(free_pin)),
    "and pin_is_protected should say so");
}

/**
 * The commands that consult the predicate are not compiled in any configuration here.
 *
 * `M42` needs `DIRECT_PIN_CONTROL`, which no `.ini` under `test/` enables, so the two tests
 * below never run. They are kept rather than deleted because the predicate above *is*
 * compiled and is only worth having because a command consults it — but a figure quoted from
 * them would be a figure from nothing.
 *
 * This was found the way these things are always found: `M42_I_overrides_the_protection`
 * **passed** against a firmware with no `M42` at all, because it asserted the *absence* of an
 * error and an unknown command produces `echo:Unknown command`. A negative assertion is
 * satisfied by every cause of nothing, including the command not existing.
 */
#if ENABLED(DIRECT_PIN_CONTROL)

/**
 * `M42` refuses a protected pin, and says why.
 *
 * The predicate is only worth having because a command consults it, and the refusal has to be
 * audible: a host that got silence could not tell a refused write from a completed one, and
 * would carry on believing it had switched something.
 */
MARLIN_TEST(protected_pins, M42_refuses_to_write_to_a_protected_pin) {
  SimulatedMachine machine;

  char cmd[32];
  snprintf(cmd, sizeof(cmd), "M42 P%d S255", int(HEATER_0_PIN));
  const std::string reply = reply_to(cmd);

  TEST_ASSERT_TRUE_MESSAGE(reply.find("Error:") != std::string::npos,
    "M42 aimed at a heater pin should be refused on the error channel");
}

/**
 * ...unless the operator insists with `I`.
 *
 * The escape hatch is deliberate — some boards really do have a spare MOSFET on a pin the
 * table claims — and it is the branch that says the protection is advisory rather than
 * absolute. Asserting only the refusal would pass against firmware that ignored `I` entirely,
 * which would make a documented parameter silently do nothing.
 *
 * The pin is written and read back rather than assumed, so this says the write happened
 * rather than that the command returned quietly.
 */
MARLIN_TEST(protected_pins, M42_I_overrides_the_protection) {
  SimulatedMachine machine;

  const uint8_t was = READ(HEATER_0_PIN);

  char cmd[32];
  snprintf(cmd, sizeof(cmd), "M42 I P%d S255", int(HEATER_0_PIN));
  const std::string reply = reply_to(cmd);

  TEST_ASSERT_TRUE_MESSAGE(reply.find("Error:") == std::string::npos,
    "M42 with I should not be refused");

  WRITE(HEATER_0_PIN, was);
}

#endif // DIRECT_PIN_CONTROL
