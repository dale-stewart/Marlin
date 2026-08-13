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
 * The limits that decide when the machine stops heating, and when it starts timing.
 *
 * Two behaviours that look unrelated but share a shape: each is a *threshold* whose
 * value is derived from a configured limit rather than chosen, and each is only
 * interesting at the boundary. Asserting that a 200 C target starts the print timer
 * says nothing — every plausible threshold would agree. Asserting that the timer starts
 * at one degree above half the cold-extrusion limit and not at half exactly pins both
 * the relation and the direction of the comparison.
 *
 *   `auto_job_over_threshold()`  the hotend threshold is EXTRUDE_MINTEMP / 2, the bed
 *                               threshold is BED_MINTEMP, and both are strict.
 *   `auto_job_check_timer()`     can_start and can_stop are independent gates, and the
 *                               two arms are mutually exclusive.
 *   `disable_all_heaters()`      every target, every duty cycle and every heater pin
 *                               goes to zero, and stays there.
 *
 * Test-HAL only: `disable_all_heaters()` is asserted against a heater the firmware is
 * actually driving, which needs the temperature ISR, and that only runs here.
 */


#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../gcode/simulated_sensors.h"
#include "src/module/temperature.h"
#include "src/module/planner.h"
#include "src/module/printcounter.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/MarlinCore.h"

namespace {

  /**
   * Put back everything these tests disturb.
   *
   * The print job timer and the heater targets are process-wide, and a test that left
   * either set would change what the next test measures — the job timer especially,
   * since `auto_job_check_timer()` reads it and writes it.
   */
  struct SavedJobState {
    celsius_t hotend, bed;
    bool was_running, was_autotemp;
    bool was_connected;
    SavedJobState() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      hotend = thermalManager.degTargetHotend(0);
      bed = TERN0(HAS_HEATED_BED, thermalManager.degTargetBed());
      was_autotemp = TERN0(AUTOTEMP, thermalManager.autotemp.enabled);
      was_running = print_job_timer.isRunning();
      print_job_timer.stop();
    }
    ~SavedJobState() {
      print_job_timer.stop();
      if (was_running) print_job_timer.start();
      thermalManager.setTargetHotend(hotend, 0);
      TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(bed));
      TERN_(AUTOTEMP, thermalManager.autotemp.enabled = was_autotemp);
      MYSERIAL1.host_connected = was_connected;
    }
  };

  // Let the machine run, the way a waiting command does.
  void time_passes_ms(const uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) { HAL_test_advance_millis(1); thermalManager.task(); }
  }

  // Nothing driven and nothing hot: the state auto_job_over_threshold() reads as "idle".
  void every_target_is_off() {
    thermalManager.setTargetHotend(0, 0);
    TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(0));
  }

  /**
   * Is the heater pin actually delivering power?
   *
   * Read from the pin and the inversion the board declares, rather than through
   * WRITE_HEATER_n() — the macro the code under test uses to switch it. Comparing the
   * pin against the same macro that wrote it would pass whatever the macro did.
   */
  #if HAS_HOTEND
    bool hotend_heater_is_powered() {
      return bool(READ(HEATER_0_PIN)) != bool(ENABLED(HEATER_0_INVERTING));
    }
  #endif
  #if HAS_HEATED_BED
    bool bed_heater_is_powered() {
      return bool(READ(HEATER_BED_PIN)) != bool(ENABLED(HEATER_BED_INVERTING));
    }
  #endif

}

#if ENABLED(PRINTJOB_TIMER_AUTOSTART)

//
// ---- Where the print job timer's threshold actually sits ----
//

/**
 * The hotend threshold is *half* the cold-extrusion limit, and it is strict.
 *
 * Half, so that a nozzle parked in standby — hot enough to keep filament soft, too cold
 * to extrude — does not look like printing. That makes the threshold a derived quantity:
 * it is not 85 because someone chose 85, it is 85 because EXTRUDE_MINTEMP is 170. So the
 * test names the relation, and checks the two targets either side of it rather than one
 * comfortably above.
 */
MARLIN_TEST(thermal_limits, the_hotend_job_threshold_is_half_the_cold_extrusion_limit) {
  SavedJobState saved;

  // Not an assertion about the firmware: a guard on this test's own arithmetic, so that
  // an odd EXTRUDE_MINTEMP does not quietly make "one degree above half" mean something
  // else than it says below.
  constexpr celsius_t threshold = (EXTRUDE_MINTEMP) / 2;
  static_assert(threshold * 2 == (EXTRUDE_MINTEMP), "EXTRUDE_MINTEMP must be even here");

  every_target_is_off();
  TEST_ASSERT_FALSE(thermalManager.auto_job_over_threshold());

  // Exactly at the threshold is *not* over it.
  thermalManager.setTargetHotend(threshold, 0);
  TEST_ASSERT_EQUAL(threshold, thermalManager.degTargetHotend(0));
  TEST_ASSERT_FALSE(thermalManager.auto_job_over_threshold());

  // One degree above it is.
  thermalManager.setTargetHotend(threshold + 1, 0);
  TEST_ASSERT_TRUE(thermalManager.auto_job_over_threshold());

  // And a standby target between the threshold and the full extrusion temperature counts
  // as printing — which is what distinguishes "half of it" from "all of it".
  thermalManager.setTargetHotend((EXTRUDE_MINTEMP) - 1, 0);
  TEST_ASSERT_TRUE(thermalManager.auto_job_over_threshold());

  every_target_is_off();
}

#if HAS_HEATED_BED

  /**
   * The bed threshold is the bed's own minimum temperature, and it is strict too.
   *
   * A bed target at or below BED_MINTEMP is not a print — it is the bed being switched
   * off, since the firmware refuses to heat below that anyway.
   */
  MARLIN_TEST(thermal_limits, the_bed_job_threshold_is_the_bed_minimum_temperature) {
    SavedJobState saved;

    every_target_is_off();
    TEST_ASSERT_FALSE(thermalManager.auto_job_over_threshold());

    thermalManager.setTargetBed(BED_MINTEMP);
    TEST_ASSERT_EQUAL(BED_MINTEMP, thermalManager.degTargetBed());
    TEST_ASSERT_FALSE(thermalManager.auto_job_over_threshold());

    thermalManager.setTargetBed((BED_MINTEMP) + 1);
    TEST_ASSERT_TRUE(thermalManager.auto_job_over_threshold());

    every_target_is_off();
  }

  // A cold hotend does not veto a hot bed, and a cold bed does not veto a hot hotend:
  // the two thresholds are an OR, so either alone is enough.
  MARLIN_TEST(thermal_limits, either_heater_alone_puts_the_job_over_the_threshold) {
    SavedJobState saved;

    every_target_is_off();
    thermalManager.setTargetBed(60);
    TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));
    TEST_ASSERT_TRUE(thermalManager.auto_job_over_threshold());

    every_target_is_off();
    thermalManager.setTargetHotend(200, 0);
    TEST_ASSERT_EQUAL(0, thermalManager.degTargetBed());
    TEST_ASSERT_TRUE(thermalManager.auto_job_over_threshold());

    every_target_is_off();
  }

#endif // HAS_HEATED_BED

//
// ---- can_start and can_stop are separate permissions ----
//

/**
 * Being over the threshold is not enough to start the timer — the caller must allow it.
 *
 * M104 sets a hot target without starting the job (`auto_job_check_timer(false, true)`),
 * because setting a temperature is not the same as beginning to print; M109, which waits
 * for that temperature, does start it. So the same thermal state produces two different
 * outcomes depending only on the permission, which is the thing worth pinning.
 */
MARLIN_TEST(thermal_limits, a_hot_target_starts_the_job_timer_only_when_starting_is_allowed) {
  SavedJobState saved;

  every_target_is_off();
  thermalManager.setTargetHotend(200, 0);
  TEST_ASSERT_TRUE(thermalManager.auto_job_over_threshold());

  print_job_timer.stop();
  thermalManager.auto_job_check_timer(false, false);
  TEST_ASSERT_FALSE(print_job_timer.isRunning());

  thermalManager.auto_job_check_timer(true, false);
  TEST_ASSERT_TRUE(print_job_timer.isRunning());

  every_target_is_off();
}

/**
 * Dropping below the threshold stops the timer — but only when stopping is allowed.
 *
 * M190 may start a job but must not end one (`auto_job_check_timer(true, false)`), so a
 * bed cooling down mid-print does not stop the clock.
 */
MARLIN_TEST(thermal_limits, a_cold_target_stops_the_job_timer_only_when_stopping_is_allowed) {
  SavedJobState saved;

  every_target_is_off();
  TEST_ASSERT_FALSE(thermalManager.auto_job_over_threshold());

  print_job_timer.start();
  TEST_ASSERT_TRUE(print_job_timer.isRunning());

  thermalManager.auto_job_check_timer(false, false);
  TEST_ASSERT_TRUE(print_job_timer.isRunning());

  thermalManager.auto_job_check_timer(true, false);
  TEST_ASSERT_TRUE(print_job_timer.isRunning());   // can_start does not stand in for can_stop

  thermalManager.auto_job_check_timer(false, true);
  TEST_ASSERT_FALSE(print_job_timer.isRunning());
}

/**
 * The two arms are exclusive: a running job that is still hot is left alone.
 *
 * M109 passes both permissions at once (`auto_job_check_timer(true, true)`). If the stop
 * arm were reachable from the over-threshold branch, every M109 during a print would
 * start the timer and immediately stop it again.
 */
MARLIN_TEST(thermal_limits, a_job_over_the_threshold_is_never_stopped_by_the_same_call) {
  SavedJobState saved;

  every_target_is_off();
  thermalManager.setTargetHotend(200, 0);

  print_job_timer.stop();
  thermalManager.auto_job_check_timer(true, true);
  TEST_ASSERT_TRUE(print_job_timer.isRunning());

  // ... and again, with the timer already running.
  thermalManager.auto_job_check_timer(true, true);
  TEST_ASSERT_TRUE(print_job_timer.isRunning());

  every_target_is_off();
}

/**
 * The same thing through the front door, so the permissions above are the ones the
 * G-code layer actually passes: M104 sets a hot target and does not start the clock,
 * M104 S0 puts it back under the threshold and does stop it.
 */
MARLIN_TEST(thermal_limits, M104_stops_a_running_job_when_the_target_goes_cold) {
  SimulatedMachine machine;
  SavedJobState saved;

  every_target_is_off();
  print_job_timer.start();
  TEST_ASSERT_TRUE(print_job_timer.isRunning());

  char hot[] = "M104 S200";
  parser.parse(hot);
  gcode.process_parsed_command(true);
  TEST_ASSERT_EQUAL(200, thermalManager.degTargetHotend(0));
  TEST_ASSERT_TRUE(print_job_timer.isRunning());     // M104 may not start, and must not stop

  char cold[] = "M104 S0";
  parser.parse(cold);
  gcode.process_parsed_command(true);
  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));
  TEST_ASSERT_FALSE(print_job_timer.isRunning());

  every_target_is_off();
}

#endif // PRINTJOB_TIMER_AUTOSTART

//
// ---- Switching everything off ----
//

/**
 * `disable_all_heaters()` is what every emergency path ends with, so "off" has to mean
 * all three things at once: no target, no duty cycle, and no power on the pin.
 *
 * Each is separately load-bearing. Clearing the target alone leaves the last computed
 * duty cycle in place until the next control pass; clearing the duty cycle alone leaves
 * the pin high for the rest of the current PWM period; clearing both but leaving the
 * target set means the very next control pass switches the heater back on. The last of
 * those is checked by letting time pass afterwards and requiring the heater to stay off.
 *
 * The duty cycles asserted here are ones the firmware itself computed — the hotend is
 * cold and its target is 200, so the PID is asking for power — rather than values the
 * test wrote in.
 */
MARLIN_TEST(thermal_limits, disabling_all_heaters_clears_every_target_duty_cycle_and_pin) {
  SimulatedMachine machine;
  SavedJobState saved;
  SimulatedSensors sensors;

  // A cold machine with hot targets: the controllers will ask for full power.
  SimulatedSensors::hotend_reads(25.0f);
  thermalManager.setTargetHotend(200, 0);
  TERN_(AUTOTEMP, thermalManager.autotemp.enabled = true);
  #if HAS_HEATED_BED
    SimulatedSensors::bed_reads(25.0f);
    thermalManager.setTargetBed(60);
  #endif

  // Long enough for the bed's bang-bang check, which only reconsiders every
  // control period, to have run at least once — see BED_CONTROL_PERIOD_MS.
  time_passes_ms(BED_CONTROL_PERIOD_MS + 1000);

  TEST_ASSERT_TRUE(thermalManager.temp_hotend[0].soft_pwm_amount > 0);
  #if HAS_HEATED_BED
    TEST_ASSERT_TRUE(thermalManager.temp_bed.soft_pwm_amount > 0);
  #endif

  // Put both pins in the state a heater that is on would leave them, so that a
  // disable_all_heaters() which forgets to write them can be told from one that does.
  WRITE_HEATER_0(HIGH);
  TEST_ASSERT_TRUE(hotend_heater_is_powered());
  #if HAS_HEATED_BED
    WRITE_HEATER_BED(HIGH);
    TEST_ASSERT_TRUE(bed_heater_is_powered());
  #endif

  thermalManager.disable_all_heaters();

  TEST_ASSERT_EQUAL(0, thermalManager.degTargetHotend(0));
  TEST_ASSERT_EQUAL(0, thermalManager.temp_hotend[0].soft_pwm_amount);
  TEST_ASSERT_FALSE(hotend_heater_is_powered());
  #if HAS_HEATED_BED
    TEST_ASSERT_EQUAL(0, thermalManager.degTargetBed());
    TEST_ASSERT_EQUAL(0, thermalManager.temp_bed.soft_pwm_amount);
    TEST_ASSERT_FALSE(bed_heater_is_powered());
  #endif

  // Whatever the planner was going to do with the nozzle temperature, it is not doing
  // it now. (Redundant in this configuration — setTargetHotend(0) clears the same flag —
  // but it is part of the function's contract, not a side effect to rely on.)
  #if ENABLED(AUTOTEMP)
    TEST_ASSERT_FALSE(thermalManager.autotemp.enabled);
  #endif
}

/**
 * ... and it stays off.
 *
 * This is the assertion that distinguishes clearing the *target* from clearing only the
 * visible outputs: with a target still set, the next control pass would drive the pin
 * high again within a fraction of a second.
 */
MARLIN_TEST(thermal_limits, a_disabled_heater_is_not_switched_back_on_by_the_control_loop) {
  SimulatedMachine machine;
  SavedJobState saved;
  SimulatedSensors sensors;

  SimulatedSensors::hotend_reads(25.0f);
  thermalManager.setTargetHotend(200, 0);
  #if HAS_HEATED_BED
    SimulatedSensors::bed_reads(25.0f);
    thermalManager.setTargetBed(60);
  #endif
  time_passes_ms(500);
  TEST_ASSERT_TRUE(thermalManager.temp_hotend[0].soft_pwm_amount > 0);

  thermalManager.disable_all_heaters();

  bool ever_powered = false;
  for (uint32_t i = 0; i < BED_CONTROL_PERIOD_MS + 1000; i++) {
    HAL_test_advance_millis(1);
    thermalManager.task();
    if (hotend_heater_is_powered()) ever_powered = true;
    #if HAS_HEATED_BED
      if (bed_heater_is_powered()) ever_powered = true;
    #endif
  }
  TEST_ASSERT_FALSE(ever_powered);
  TEST_ASSERT_EQUAL(0, thermalManager.temp_hotend[0].soft_pwm_amount);
  #if HAS_HEATED_BED
    TEST_ASSERT_EQUAL(0, thermalManager.temp_bed.soft_pwm_amount);
  #endif
}

