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
 * What the machine does when a temperature sensor stops making sense.
 *
 * A thermistor that falls out of the heater block reads room temperature for ever, and the
 * control loop answers by applying full power indefinitely. A thermistor whose wires short
 * reads off the top of the scale. Neither reading is a temperature — they are the two ways a
 * sensor fails — and the firmware has to notice and cut the power, because everything
 * downstream of the sensor is now working from a number that means nothing.
 *
 * `013-bogus_temp_grace` is why this file can exist at all. `_temp_error()` normally ends in
 * `Marlin::kill()`, which does not return, so a test could reach these checks but never assert
 * after them and their mutants were detectable only by hanging — register #19, sixty-three
 * survivors. `BOGUS_TEMPERATURE_GRACE_PERIOD` is Marlin's own seam for exactly that: within
 * the grace period a temperature error disables the heaters and *returns*. Nothing is stubbed
 * and no production code changed.
 *
 * What is asserted is therefore the half that matters most and was never covered: that the
 * fault is **detected** and the heaters are **switched off**. The killing that follows stays
 * untested and stays recorded as #19.
 *
 * Two things about the fixture, both of which cost time to establish and neither of which is
 * guessable from the code.
 *
 * **`Temperature::init()` does run here**, from `SimulatedHardware::ensure_ready()`. That
 * matters because `init()` is what narrows `temp_range[]` from the thermistor table's own ends
 * to the configured `HEATER_0_MINTEMP` / `HEATER_0_MAXTEMP`, so the limits under test are the
 * machine's stated ones and the boundary can be bracketed properly. (`CLAUDE.md` carried a
 * gotcha saying `init()` crashes with SIGFPE; that was true under the older harness and is not
 * true now. It has been corrected.)
 *
 * **The reading takes about 300 ms of simulated time to arrive.** The ADC is oversampled 16
 * times and the conversion only lands when a full set is in, so a sensor changed and then
 * checked 50 ms later still reads the old value — which looks exactly like a fault that was
 * not detected. Every test here allows the reading to flush before asserting anything.
 */

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/kill_button.h"
#include "../gcode/serial_capture.h"
#include "../gcode/simulated_sensors.h"
#include "src/module/temperature.h"
#include "src/module/printcounter.h"
#include "src/MarlinCore.h"

#if HAS_HOTEND && BOGUS_TEMPERATURE_GRACE_PERIOD

namespace {

  // The machine's own stated limits, so the tests say what the configuration says.
  constexpr celsius_float_t HOTEND_MAX = HEATER_0_MAXTEMP, HOTEND_MIN = HEATER_0_MINTEMP;

  // The coldest reading this build actually tolerates. One count above the configured
  // minimum — see `the_cold_limit_sits_one_reading_above_the_configured_minimum`.
  constexpr celsius_float_t COLDEST_ALLOWED = HOTEND_MIN + 1.0f;
  #if HAS_HEATED_BED
    constexpr celsius_float_t BED_MAX = BED_MAXTEMP, BED_MIN = BED_MINTEMP;
  #endif

  // Long enough for a changed sensor to reach the firmware: 16 oversampled conversions.
  constexpr uint32_t SENSOR_FLUSH_MS = 400;

  struct SavedTargets {
    celsius_t hotend, bed;
    bool was_running, was_connected;
    SavedTargets() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      hotend = thermalManager.degTargetHotend(0);
      bed = TERN0(HAS_HEATED_BED, thermalManager.degTargetBed());
      was_running = print_job_timer.isRunning();
      print_job_timer.stop();
    }
    ~SavedTargets() {
      print_job_timer.stop();
      if (was_running) print_job_timer.start();
      thermalManager.setTargetHotend(hotend, 0);
      TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(bed));
      MYSERIAL1.host_connected = was_connected;
    }
  };

  // Let the machine run its temperature task, the way a waiting command does.
  void time_passes_ms(const uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) { HAL_test_advance_millis(1); thermalManager.task(); }
  }

  /**
   * Is the heater pin actually delivering power?
   *
   * Read from the pin and the board's own inversion setting rather than through
   * `WRITE_HEATER_n()`, the macro the code under test writes it with — comparing a pin
   * against the macro that set it would agree whatever the macro did.
   */
  bool hotend_heater_is_powered() {
    return bool(READ(HEATER_0_PIN)) != bool(ENABLED(HEATER_0_INVERTING));
  }
  #if HAS_HEATED_BED
    bool bed_heater_is_powered() {
      return bool(READ(HEATER_BED_PIN)) != bool(ENABLED(HEATER_BED_INVERTING));
    }
  #endif

  // A machine heating normally: a hot target, a cold nozzle, and the controller asking
  // for power. Everything below starts here, so "the heater went off" means something.
  void a_machine_that_is_heating() {
    SimulatedSensors::hotend_reads(25.0f);
    thermalManager.setTargetHotend(200, 0);
    #if HAS_HEATED_BED
      SimulatedSensors::bed_reads(25.0f);
      thermalManager.setTargetBed(60);
    #endif
    time_passes_ms(BED_CONTROL_PERIOD_MS + 1000);
  }

}

// ---------------------------------------------------------------------------
// A sensor that has stopped making sense
// ---------------------------------------------------------------------------

/**
 * A reading above the configured maximum switches the heater off and keeps it off.
 *
 * This is a shorted thermistor, and it is the dangerous one: the reading says the nozzle is
 * far hotter than the machine allows, so the block may be at a temperature that will set fire
 * to filament and the sensor can no longer be believed about it. The answer is to stop, not
 * to regulate.
 *
 * Clearing the *target* as well as the pin is the assertion worth having: clearing only the
 * pin leaves the next control pass free to switch it straight back on. Time is allowed to
 * pass afterwards to say so.
 */
MARLIN_TEST(temperature_errors, a_hotend_above_its_maximum_switches_the_heater_off) {
  SimulatedMachine machine;
  SavedTargets saved;
  SimulatedSensors sensors;

  a_machine_that_is_heating();
  TEST_ASSERT_TRUE_MESSAGE(thermalManager.temp_hotend[0].soft_pwm_amount > 0,
    "the fixture should have the hotend heating before the sensor fails");

  SimulatedSensors::hotend_reads_at_least(HOTEND_MAX + 5.0f);
  time_passes_ms(SENSOR_FLUSH_MS);

  TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetHotend(0),
    "a hotend above its maximum should clear the target, not merely the pin");
  TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.temp_hotend[0].soft_pwm_amount,
    "and the duty cycle with it");
  TEST_ASSERT_FALSE_MESSAGE(hotend_heater_is_powered(), "and the heater pin");

  // ...and the control loop does not put it back.
  time_passes_ms(500);
  TEST_ASSERT_FALSE_MESSAGE(hotend_heater_is_powered(),
    "a heater switched off by a sensor fault should stay off");
}

/**
 * A reading below the configured minimum does the same.
 *
 * This is a disconnected thermistor — an open circuit reads as infinitely cold — and it is
 * the failure the protection exists for: the control loop sees a nozzle that will not heat
 * and answers with full power for ever. It is caught by a different branch from the one
 * above, with its own comparison and its own direction, so it needs its own test; a machine
 * that caught one and not the other would look protected.
 */
MARLIN_TEST(temperature_errors, a_hotend_below_its_minimum_switches_the_heater_off) {
  SimulatedMachine machine;
  SavedTargets saved;
  SimulatedSensors sensors;

  a_machine_that_is_heating();
  TEST_ASSERT_TRUE(thermalManager.temp_hotend[0].soft_pwm_amount > 0);

  SimulatedSensors::hotend_reads_below(HOTEND_MIN);
  time_passes_ms(SENSOR_FLUSH_MS);

  TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetHotend(0),
    "a disconnected hotend sensor should clear the target");
  TEST_ASSERT_FALSE_MESSAGE(hotend_heater_is_powered(), "and switch the heater off");
}

/**
 * Just inside the limits is not a fault — the other side of both brackets.
 *
 * Without this, firmware that shut the machine down on every pass would satisfy both tests
 * above, and a printer that could not hold a temperature for a second would look like one
 * with excellent thermal protection. The readings used are as close to the limits as the
 * sensor can express, so this pins where the boundary is rather than merely that there is
 * one somewhere.
 */
MARLIN_TEST(temperature_errors, a_reading_just_inside_the_limits_is_left_alone) {
  SimulatedMachine machine;
  SavedTargets saved;
  SimulatedSensors sensors;

  a_machine_that_is_heating();

  SimulatedSensors::hotend_reads_below(HOTEND_MAX);
  time_passes_ms(SENSOR_FLUSH_MS);
  TEST_ASSERT_EQUAL_MESSAGE(200, thermalManager.degTargetHotend(0),
    "a nozzle just under its maximum is a printer working normally");

  SimulatedSensors::hotend_reads_at_least(COLDEST_ALLOWED);
  time_passes_ms(SENSOR_FLUSH_MS);
  TEST_ASSERT_EQUAL_MESSAGE(200, thermalManager.degTargetHotend(0),
    "and so is one just above its minimum");
}

/**
 * The cold boundary, bracketed to one representable reading — and it is not where the
 * configuration says.
 *
 * `HEATER_0_MINTEMP` is 5, and a nozzle reading exactly 5.00 C is shut down; 6.00 C is not.
 * The comparison is on raw counts rather than degrees: `init()` walks `raw_min` down from the
 * table's cold end in steps of `OVERSAMPLENR`, starting from a value that is not a multiple of
 * it, so `raw_min` never lands on a reading the ADC can actually produce and the effective
 * limit sits one count inside the configured one.
 *
 * Harmless in practice — 5 C is below any room a printer lives in — and recorded here rather
 * than in the defect register because it is a property of where the boundary quantises, not a
 * mistake in the comparison. What matters for the tests is that it is *pinned*: this is the
 * tightest bracket the sensor can express, so a mutant that moves the limit by one count in
 * either direction fails it.
 */
MARLIN_TEST(temperature_errors, the_cold_limit_sits_one_reading_above_the_configured_minimum) {
  SimulatedMachine machine;
  SavedTargets saved;
  SimulatedSensors sensors;

  a_machine_that_is_heating();
  SimulatedSensors::hotend_reads_at_least(HOTEND_MIN);          // exactly 5.00 C
  time_passes_ms(SENSOR_FLUSH_MS);
  TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetHotend(0),
    "a reading of exactly the configured minimum is treated as a fault");

  a_machine_that_is_heating();
  SimulatedSensors::hotend_reads_at_least(COLDEST_ALLOWED);     // the next reading up
  time_passes_ms(SENSOR_FLUSH_MS);
  TEST_ASSERT_EQUAL_MESSAGE(200, thermalManager.degTargetHotend(0),
    "and the next representable reading above it is not");
}

/**
 * The cold check only applies to a heater that is switched on.
 *
 * A nozzle nobody has asked to heat is *supposed* to be cold, and a machine that called that
 * a sensor fault would refuse to start from cold — which is every print. The guard is
 * `heater_on`, and it is easy to lose.
 *
 * The observable is the *bed*: with the hotend's target already at zero there is nothing left
 * of the hotend to switch off, so a wrongly-fired error shows up as `disable_all_heaters()`
 * taking the bed down with it. That is a real consequence rather than a proxy — it is how a
 * cold nozzle would abandon a print that was heating its bed.
 */
#if HAS_HEATED_BED
  MARLIN_TEST(temperature_errors, a_cold_reading_is_ignored_while_the_hotend_is_off) {
    SimulatedMachine machine;
    SavedTargets saved;
    SimulatedSensors sensors;

    SimulatedSensors::bed_reads(25.0f);
    thermalManager.setTargetBed(60);
    thermalManager.setTargetHotend(0, 0);          // nobody has asked for a hot nozzle
    time_passes_ms(BED_CONTROL_PERIOD_MS + 1000);
    TEST_ASSERT_TRUE_MESSAGE(thermalManager.temp_bed.soft_pwm_amount > 0,
      "the fixture should have the bed heating");

    SimulatedSensors::hotend_reads_below(HOTEND_MIN);
    time_passes_ms(SENSOR_FLUSH_MS);

    TEST_ASSERT_EQUAL_MESSAGE(60, thermalManager.degTargetBed(),
      "a cold reading from a hotend that is switched off should not stop the bed");
  }
#endif

/**
 * The bed is checked too, and by its own code.
 *
 * `updateTemperaturesFromRawValues()` has a separate block per sensor — its own range, its
 * own comparison, its own heater id — so covering the hotend leaves the bed's copy
 * unasserted. The failure that matters is the same one: a bed heater held at full power by a
 * sensor that has stopped reporting, underneath a printed part.
 */
#if HAS_HEATED_BED
  MARLIN_TEST(temperature_errors, a_bed_above_its_maximum_switches_the_bed_off) {
    SimulatedMachine machine;
    SavedTargets saved;
    SimulatedSensors sensors;

    a_machine_that_is_heating();
    TEST_ASSERT_TRUE_MESSAGE(thermalManager.temp_bed.soft_pwm_amount > 0,
      "the fixture should have the bed heating before the sensor fails");

    SimulatedSensors::bed_reads_at_least(BED_MAX + 1.0f);
    time_passes_ms(SENSOR_FLUSH_MS);

    TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetBed(),
      "a bed above its maximum should clear the bed target");
    TEST_ASSERT_FALSE_MESSAGE(bed_heater_is_powered(), "and switch the bed heater off");
  }

  /**
   * The bed's cold check is its own too, and it is the one with the extra condition.
   *
   * A bed reading below its minimum with the heater on is a disconnected bed thermistor —
   * the same failure as the hotend's, on the heater with the most thermal mass and the most
   * of a printed part sitting on it. It is a separate line from the bed's maximum check,
   * with the comparison the other way round and a `target > 0` guard the maximum check does
   * not have, so covering the hot side leaves it entirely unasserted.
   */
  MARLIN_TEST(temperature_errors, a_bed_below_its_minimum_switches_the_bed_off) {
    SimulatedMachine machine;
    SavedTargets saved;
    SimulatedSensors sensors;

    a_machine_that_is_heating();
    TEST_ASSERT_TRUE(thermalManager.temp_bed.soft_pwm_amount > 0);

    SimulatedSensors::bed_reads_below(BED_MIN);
    time_passes_ms(SENSOR_FLUSH_MS);

    TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetBed(),
      "a disconnected bed sensor should clear the bed target");
    TEST_ASSERT_FALSE_MESSAGE(bed_heater_is_powered(), "and switch the bed heater off");
  }

  /**
   * ...but only while the bed is switched on.
   *
   * The cold check is guarded on `target > 0` for the same reason the hotend's is: a bed
   * nobody has asked to heat is supposed to be cold. The observable here is the *hotend*,
   * for the same reason the mirrored test uses the bed — with the bed's target already at
   * zero there is nothing of the bed left to switch off.
   */
  MARLIN_TEST(temperature_errors, a_cold_bed_is_ignored_while_the_bed_is_off) {
    SimulatedMachine machine;
    SavedTargets saved;
    SimulatedSensors sensors;

    SimulatedSensors::hotend_reads(25.0f);
    thermalManager.setTargetHotend(200, 0);
    thermalManager.setTargetBed(0);                // nobody has asked for a warm bed
    time_passes_ms(BED_CONTROL_PERIOD_MS + 1000);
    TEST_ASSERT_TRUE(thermalManager.temp_hotend[0].soft_pwm_amount > 0);

    SimulatedSensors::bed_reads_below(BED_MIN);
    time_passes_ms(SENSOR_FLUSH_MS);

    TEST_ASSERT_EQUAL_MESSAGE(200, thermalManager.degTargetHotend(0),
      "a cold reading from a bed that is switched off should not stop the hotend");
  }

  /**
   * ...and a bed fault takes the hotend down with it.
   *
   * `_temp_error()` calls `disable_all_heaters()`, not "disable the heater that failed", and
   * that is deliberate: a machine with one sensor it cannot believe is a machine that should
   * not be applying power anywhere. Asserting only the failed heater would pass against
   * firmware that left the nozzle at full power with the bed's sensor shorted.
   */
  MARLIN_TEST(temperature_errors, a_bed_fault_stops_the_hotend_as_well) {
    SimulatedMachine machine;
    SavedTargets saved;
    SimulatedSensors sensors;

    a_machine_that_is_heating();
    TEST_ASSERT_TRUE(thermalManager.temp_hotend[0].soft_pwm_amount > 0);

    SimulatedSensors::bed_reads_at_least(BED_MAX + 1.0f);
    time_passes_ms(SENSOR_FLUSH_MS);

    TEST_ASSERT_EQUAL_MESSAGE(0, thermalManager.degTargetHotend(0),
      "a sensor the machine cannot believe should stop every heater, not just its own");
  }
#endif

// ---------------------------------------------------------------------------
// What the host is told — and why no test can assert it
// ---------------------------------------------------------------------------

/**
 * The report is a once-per-process event, so at most one test in the whole run could ever
 * see it — and which one depends on link order. Defect #53.
 *
 * This was worth chasing because retiring the `kill()` blocker (register #19) looked as though
 * it should have opened the message up: in the default build the *first* temperature error
 * reports and then calls `loud_kill()`, and `kill()` now returns once an operator presses the
 * button. The shutdown is asserted above under `013`; the message should have been assertable
 * here.
 *
 * It is not, and the reason is neither `kill()` nor the grace period:
 *
 *     void Temperature::_temp_error(...) {
 *       static uint8_t killed = 0;
 *       if (marlin.isRunning() && killed == TERN(HAS_BOGUS_TEMPERATURE_GRACE_PERIOD, 2, 0)) {
 *         ... the report ...
 *
 * `killed` is a function-local static. Once anything anywhere in the process has taken a
 * temperature error, every later one is silent for the rest of the run. A probe printing
 * `killed` on entry reports **1** by the time this file executes, so the one report this
 * process was ever going to make had already been spent by an earlier test.
 *
 * A test that only passes when it happens to run first is worse than no test: it would pass
 * today, fail the day a file is added ahead of it, and the failure would look like a defect in
 * the code rather than in the ordering. So the tests that were written here are deleted rather
 * than kept green by arranging the order, and the finding is recorded instead.
 *
 * Under `013` the same static is what makes the report unreachable from the other side: the
 * grace period moves the reporting threshold to `killed == 2`, and `killed` only advances past
 * 1 once the grace expires, which nothing in a test run waits an hour for.
 *
 * What would fix it is a production change — the flag wants to be resettable, or the reporting
 * wants to be separable from the once-only kill — and that is behind the frontier.
 */

#endif // HAS_HOTEND && BOGUS_TEMPERATURE_GRACE_PERIOD

