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
 * Temperature, measured rather than asserted.
 *
 * Everything here needs `Temperature::isr()` to actually run, which it only does under
 * the test HAL: the temperature timer is armed by `Temperature::init()` and fires
 * because simulated time crossed its compare value. Nothing in this file calls the ISR.
 *
 * Test-HAL only: under HAL/LINUX the timer is a POSIX signal that a test build never
 * starts, so these would wait forever.
 */


#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_heaters.h"
#include "../gcode/simulated_sensors.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/temperature.h"
#include <string.h>

namespace {

  struct SavedTargets {
    celsius_t hotend, bed;
    SavedTargets() {
      hotend = thermalManager.degTargetHotend(0);
      bed = TERN0(HAS_HEATED_BED, thermalManager.degTargetBed());
    }
    ~SavedTargets() {
      thermalManager.setTargetHotend(hotend, 0);
      TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(bed));
    }
  };

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  // Let the machine run for a while, the way a waiting command does.
  void time_passes_ms(const uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) { HAL_test_advance_millis(1); thermalManager.task(); }
  }

  /**
   * Long enough for a changed input to become the reading.
   *
   * The ISR sums OVERSAMPLENR conversions of each sensor before a set of raw values is
   * ready, so for roughly one sampling period after an input changes the sum is part
   * old and part new and the reading sits between the two. Two full periods is enough
   * to be sure the whole sum is the new value; at TEMP_TIMER_FREQUENCY that is a couple
   * of hundred milliseconds.
   */
  void the_sensors_catch_up() { time_passes_ms(400); }

  celsius_float_t hotend_conversion_of(const uint16_t code) {
    return thermalManager.analog_to_celsius_hotend(raw_adc_t(code * OVERSAMPLENR), 0);
  }

}

//
// ---- The ADC pipeline ----
//

/**
 * A reading is a consequence of a count on a pin.
 *
 * This is the whole reason the ISR has to run: `Temperature::isr()` takes OVERSAMPLENR
 * conversions of each sensor and `Temperature::task()` converts the sum through the
 * thermistor table. Driving the pin and advancing the clock exercises both, and the
 * temperature that comes out is the one the table says it should be — not one the test
 * wrote into `temp_hotend[0].celsius`.
 */
MARLIN_TEST(simulated_temperature, a_driven_adc_count_becomes_a_reading) {
  SimulatedMachine machine;
  SimulatedSensors sensors;

  const uint16_t code = 600;
  SimulatedHardware::drive_adc(TEMP_0_PIN, code);
  the_sensors_catch_up();

  TEST_ASSERT_EQUAL(code * OVERSAMPLENR, thermalManager.rawHotendTemp(0));
  TEST_ASSERT_EQUAL_FLOAT(hotend_conversion_of(code), thermalManager.degHotend(0));
}

// Two different counts give two different readings, in the direction the sensor works:
// a thermistor divider reads a lower count when it is hotter.
MARLIN_TEST(simulated_temperature, a_lower_count_reads_hotter) {
  SimulatedMachine machine;
  SimulatedSensors sensors;

  SimulatedHardware::drive_adc(TEMP_0_PIN, 700);
  the_sensors_catch_up();
  const celsius_float_t cooler = thermalManager.degHotend(0);

  SimulatedHardware::drive_adc(TEMP_0_PIN, 400);
  the_sensors_catch_up();
  const celsius_float_t hotter = thermalManager.degHotend(0);

  TEST_ASSERT_TRUE(hotter > cooler);
}

/**
 * Nothing happens while the clock stands still.
 *
 * That is the property the test HAL exists for, and it is worth pinning here because
 * the ADC pipeline is the one part of the machine that used to be inert for the wrong
 * reason — because its timer was never started rather than because no time had passed.
 */
MARLIN_TEST(simulated_temperature, a_new_count_is_not_read_until_time_advances) {
  SimulatedMachine machine;
  SimulatedSensors sensors;

  SimulatedHardware::drive_adc(TEMP_0_PIN, 500);
  the_sensors_catch_up();
  const celsius_float_t before = thermalManager.degHotend(0);

  SimulatedHardware::drive_adc(TEMP_0_PIN, 900);
  thermalManager.task();                                   // no time has passed
  TEST_ASSERT_EQUAL_FLOAT(before, thermalManager.degHotend(0));

  the_sensors_catch_up();
  TEST_ASSERT_TRUE(thermalManager.degHotend(0) < before);
}

//
// ---- A heater that responds to the firmware ----
//

// With a target set, the firmware powers the heater and the heater gets hot. Nothing
// here writes a temperature: the reading rises because the PID switched the pin on.
MARLIN_TEST(simulated_temperature, a_heater_warms_up_when_the_firmware_powers_it) {
  SimulatedMachine machine;
  SavedTargets targets;
  SimulatedHotend hotend;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  the_sensors_catch_up();
  const celsius_float_t cold = thermalManager.degHotend(0);

  thermalManager.setTargetHotend(200, 0);
  time_passes_ms(4000);

  TEST_ASSERT_TRUE(thermalManager.degHotend(0) > cold + 50.0f);
}

// And cools when the target is removed.
MARLIN_TEST(simulated_temperature, a_heater_cools_when_the_firmware_stops_powering_it) {
  SimulatedMachine machine;
  SavedTargets targets;
  SimulatedHotend hotend;

  thermalManager.setTargetHotend(150, 0);
  time_passes_ms(6000);
  const celsius_float_t hot = thermalManager.degHotend(0);
  TEST_ASSERT_TRUE(hot > 100.0f);

  thermalManager.setTargetHotend(0, 0);
  time_passes_ms(8000);
  TEST_ASSERT_TRUE(thermalManager.degHotend(0) < hot - 20.0f);
}

//
// ---- M109 ----
//

/**
 * M109 waits for a temperature that has to be reached.
 *
 * Until now this could only be tested with the sensor already reading above the target,
 * so the command returned without ever waiting and the residency logic never ran. With
 * a heater that responds to the firmware, M109 starts cold, the PID drives it up, and
 * the command returns because the hotend really got there.
 */
MARLIN_TEST(simulated_temperature, M109_waits_until_the_hotend_reaches_its_target) {
  SimulatedMachine machine;
  SavedTargets targets;
  SimulatedHotend hotend;

  hotend.starts_at(SimulatedHeater::AMBIENT_C);
  the_sensors_catch_up();
  TEST_ASSERT_TRUE(thermalManager.degHotend(0) < 100.0f);

  host_sends("M109 S200");

  TEST_ASSERT_EQUAL(200, thermalManager.degTargetHotend(0));
  TEST_ASSERT_FLOAT_WITHIN(TEMP_HYSTERESIS, 200.0f, thermalManager.degHotend(0));

  thermalManager.setTargetHotend(0, 0);
}

/**
 * ... and holds on for the residency time once it gets there.
 *
 * M109 does not return the moment the reading touches the target; it waits for the
 * hotend to sit within TEMP_WINDOW of it for TEMP_RESIDENCY_TIME, so a nozzle that
 * briefly overshoots on the way past is not treated as ready. That is measurable here
 * because the clock is simulated: the command must consume at least the residency time.
 */
MARLIN_TEST(simulated_temperature, M109_holds_the_hotend_at_the_target_for_the_residency_time) {
  SimulatedMachine machine;
  SavedTargets targets;
  SimulatedHotend hotend;

  // Short of the target by more than TEMP_WINDOW, so the residency timer starts on a
  // later pass of the wait loop rather than on the first one. See the test below for
  // why that distinction matters.
  hotend.starts_at(197.0f);
  the_sensors_catch_up();
  TEST_ASSERT_TRUE(ABS(200.0f - thermalManager.degHotend(0)) > float(TEMP_WINDOW));

  const millis_t started = millis();
  host_sends("M109 S200");
  const millis_t took = millis() - started;

  // Bracketed from both sides, but the two sides are not equally tight and it is worth
  // saying so. "At least the residency time" is satisfied by a wait of any length at all,
  // including one that never ends on a real printer, so an upper bound is needed — but the
  // measured wait here is ~45 s, of which only 10 s is residency and the rest is the
  // simulated heater climbing the last 3 C. The bound therefore catches a wait that runs
  // away, not a residency of the wrong size. Pinning the residency itself needs the
  // *difference* between an undisturbed wait and one interrupted by a dip out of the
  // hysteresis, which is a separate test and is not written yet.
  TEST_ASSERT_TRUE_MESSAGE(took >= SEC_TO_MS(TEMP_RESIDENCY_TIME),
    "M109 should hold at the target for the residency time");
  TEST_ASSERT_TRUE_MESSAGE(took < SEC_TO_MS(TEMP_RESIDENCY_TIME) * 12,
    "and should return once it has, not go on waiting");
  TEST_ASSERT_FLOAT_WITHIN(TEMP_HYSTERESIS, 200.0f, thermalManager.degHotend(0));

  thermalManager.setTargetHotend(0, 0);
}

/**
 * LEGACY-BEHAVIOR: M109 skips the residency wait entirely if the hotend is already
 * within TEMP_WINDOW of the target on the first pass of the wait loop.
 *
 * `wait_for_hotend()` starts the residency timer at
 * `now + (first_loop ? SEC_TO_MS(TEMP_RESIDENCY_TIME) / 3 : 0)`, and then tests it with
 * the three-argument `PENDING(now, start, interval)`, which is `(now - start) <
 * interval` in unsigned arithmetic. A `start` in the future makes `now - start` wrap to
 * near 2^32, which is not less than the interval, so the loop condition is false on the
 * very next pass and the command returns at once.
 *
 * On the first pass only — reach the window one pass later and the offset is zero, the
 * subtraction does not wrap, and the full residency time is observed (the test above).
 * So a hotend that happens to be at temperature when M109 arrives is not held at all.
 *
 * Recorded, not fixed: see docs/defect-register.md.
 */
MARLIN_TEST(simulated_temperature, M109_does_not_hold_a_hotend_that_is_already_at_the_target) {
  SimulatedMachine machine;
  SavedTargets targets;
  SimulatedSensors sensors;   // a steady reading, so nothing drifts out of the window

  // Just under the target: inside TEMP_WINDOW, and not so high that M109 takes the
  // "we are cooling, do not wait" exit for a different reason.
  const celsius_float_t at_start = SimulatedSensors::hotend_reads_below(200.0f);
  TEST_ASSERT_TRUE(200.0f - at_start < float(TEMP_WINDOW));

  const millis_t started = millis();
  host_sends("M109 S200");
  const millis_t took = millis() - started;

  // The other side of the same bracket, and the tighter one. "Less than the residency time"
  // is satisfied by a wait of nine seconds out of ten — and by the mutant that starts the
  // timer in the *past* rather than the future, which waits two thirds of it. What this
  // behaviour actually is, is a command that returns at once, so that is what is asserted.
  TEST_ASSERT_TRUE_MESSAGE(took < SEC_TO_MS(TEMP_RESIDENCY_TIME) / 4,
    "an already-hot nozzle should not be waited on at all");

  thermalManager.setTargetHotend(0, 0);
}

// Cooling is not waited for: a print that ends with M109 S0 should not sit there until
// the nozzle is cold. The target changes and the command returns.
MARLIN_TEST(simulated_temperature, M109_does_not_wait_for_a_hotend_that_is_cooling) {
  SimulatedMachine machine;
  SavedTargets targets;
  SimulatedHotend hotend;

  hotend.starts_at(220.0f);
  the_sensors_catch_up();

  const millis_t started = millis();
  host_sends("M109 S180");
  TEST_ASSERT_TRUE(millis() - started < SEC_TO_MS(TEMP_RESIDENCY_TIME));
  TEST_ASSERT_EQUAL(180, thermalManager.degTargetHotend(0));

  thermalManager.setTargetHotend(0, 0);
}

/**
 * M109 R waits for the hotend to come *down* to a temperature.
 *
 * S means "do not wait if we are cooling"; R means "wait either way". The cooling path
 * is a different loop with its own escape — it gives up if the hotend does not drop
 * MIN_COOLING_SLOPE_DEG within MIN_COOLING_SLOPE_TIME, so a printer with no fan is not
 * held forever. Here the hotend does cool, so the wait ends by arriving.
 */
MARLIN_TEST(simulated_temperature, M109_R_waits_for_the_hotend_to_cool_to_the_target) {
  SimulatedMachine machine;
  SavedTargets targets;
  SimulatedHotend hotend;

  hotend.starts_at(230.0f);
  the_sensors_catch_up();
  TEST_ASSERT_TRUE(thermalManager.degHotend(0) > 200.0f);

  const millis_t started = millis();
  host_sends("M109 R150");
  const millis_t took = millis() - started;

  TEST_ASSERT_TRUE(took > 0);                                  // it really waited
  TEST_ASSERT_TRUE(thermalManager.degHotend(0) < 200.0f);      // ... and it cooled
  TEST_ASSERT_EQUAL(150, thermalManager.degTargetHotend(0));

  thermalManager.setTargetHotend(0, 0);
}

#if ENABLED(PIDTEMP)

  /**
   * M303 tunes the PID by making the hotend oscillate.
   *
   * Autotune drives the heater as a relay — full on below the target, off above it —
   * and measures the period and amplitude of the resulting oscillation to derive gains.
   * It cannot be tested against a fixed reading at all: with a temperature that never
   * changes there is no oscillation to measure and the routine runs until its own
   * twenty-minute timeout. With a heater that responds to the pin it completes, and the
   * gains it produces are positive numbers derived from a real cycle.
   */
  MARLIN_TEST(simulated_temperature, M303_tunes_the_hotend_pid_from_a_real_oscillation) {
    SimulatedMachine machine;
    SavedTargets targets;
    SimulatedHotend hotend;

    const raw_pid_t was = { thermalManager.temp_hotend[0].pid.p(),
                            thermalManager.temp_hotend[0].pid.i(),
                            thermalManager.temp_hotend[0].pid.d() };

    hotend.starts_at(SimulatedHeater::AMBIENT_C);
    the_sensors_catch_up();

    host_sends("M303 E0 S180 C3 U1");

    const raw_pid_t tuned = { thermalManager.temp_hotend[0].pid.p(),
                              thermalManager.temp_hotend[0].pid.i(),
                              thermalManager.temp_hotend[0].pid.d() };
    TEST_ASSERT_TRUE(tuned.p > 0.0f);
    TEST_ASSERT_TRUE(tuned.i > 0.0f);
    TEST_ASSERT_TRUE(tuned.d > 0.0f);

    thermalManager.temp_hotend[0].pid.set(was);
    thermalManager.updatePID();
    thermalManager.setTargetHotend(0, 0);
  }

#endif // PIDTEMP

#if HAS_HEATED_BED

  //
  // ---- M190 ----
  //

  // The bed is bang-bang controlled rather than PID, so this covers the other of the
  // two heater control paths as well as the bed's own wait loop.
  MARLIN_TEST(simulated_temperature, the_bed_warms_up_when_the_firmware_powers_it) {
    SimulatedMachine machine;
    SavedTargets targets;
    SimulatedBed bed;

    bed.starts_at(SimulatedHeater::AMBIENT_C);
    the_sensors_catch_up();
    const celsius_float_t cold = thermalManager.degBed();

    thermalManager.setTargetBed(60);
    time_passes_ms(20000);

    TEST_ASSERT_TRUE(thermalManager.degBed() > cold + 5.0f);   // rising
    TEST_ASSERT_TRUE(thermalManager.degBed() < 60.0f);          // and not there yet

    thermalManager.setTargetBed(0);
  }

  MARLIN_TEST(simulated_temperature, M190_waits_until_the_bed_reaches_its_target) {
    SimulatedMachine machine;
    SavedTargets targets;
    SimulatedBed bed;

    bed.starts_at(50.0f);                              // short of the target
    the_sensors_catch_up();
    TEST_ASSERT_TRUE(thermalManager.degBed() < 60.0f - float(TEMP_BED_WINDOW));

    const millis_t started = millis();
    host_sends("M190 S60");
    const millis_t took = millis() - started;

    TEST_ASSERT_EQUAL(60, thermalManager.degTargetBed());
    TEST_ASSERT_FLOAT_WITHIN(TEMP_BED_HYSTERESIS, 60.0f, thermalManager.degBed());
    TEST_ASSERT_TRUE(took >= SEC_TO_MS(TEMP_BED_RESIDENCY_TIME));

    thermalManager.setTargetBed(0);
  }

  // As with the hotend, asking the bed to cool does not hold up the job.
  MARLIN_TEST(simulated_temperature, M190_does_not_wait_for_a_bed_that_is_cooling) {
    SimulatedMachine machine;
    SavedTargets targets;
    SimulatedBed bed;

    bed.starts_at(90.0f);
    the_sensors_catch_up();

    const millis_t started = millis();
    host_sends("M190 S60");
    TEST_ASSERT_TRUE(millis() - started < SEC_TO_MS(TEMP_BED_RESIDENCY_TIME));
    TEST_ASSERT_EQUAL(60, thermalManager.degTargetBed());

    thermalManager.setTargetBed(0);
  }

  MARLIN_TEST(simulated_temperature, M190_R_waits_for_the_bed_to_cool_to_the_target) {
    SimulatedMachine machine;
    SavedTargets targets;
    SimulatedBed bed;

    bed.starts_at(70.0f);
    the_sensors_catch_up();
    TEST_ASSERT_TRUE(thermalManager.degBed() > 65.0f);

    host_sends("M190 R60");

    TEST_ASSERT_EQUAL(60, thermalManager.degTargetBed());
    TEST_ASSERT_TRUE(thermalManager.degBed() < 65.0f);

    thermalManager.setTargetBed(0);
  }

#endif // HAS_HEATED_BED

