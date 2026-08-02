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
#pragma once

/**
 * A machine that actually moves.
 *
 * Most of this rescue could not test anything that waits for hardware: homing, dwell,
 * arcs, M400 and a real M109 all wait for an interrupt that never fires, because the
 * LINUX HAL's interrupts are POSIX timer signals and `main()` — the only thing that
 * initialises them — is excluded from a unit test build.
 *
 * The fix is smaller than it looks. The timers only need initialising, not *running*:
 * once HAL_timer_init() has set them up, the interrupt handlers can be called directly
 * and the real stepper ISR steps the real planner. Bringing the board up is shared with
 * every other fixture that needs working hardware, so it lives in simulated_hardware.h;
 * under the LINUX HAL this fixture then masks the signals so nothing fires behind the
 * test's back, and lets the test say when time passes.
 *
 * That is the property worth keeping: time is explicit. `run_until_idle()` is a loop
 * the test controls, not a sleep, so the tests stay fast and repeatable.
 *
 * Validated against a known result rather than against itself: a 1 mm move at
 * 80 steps/mm leaves the stepper at exactly 80 steps.
 */

#include "src/inc/MarlinConfig.h"
#include "src/module/stepper.h"
#include "src/module/planner.h"
#include "src/module/motion.h"
#include "src/module/temperature.h"
#include "src/MarlinCore.h"
#include "simulated_hardware.h"

class SimulatedMachine {
public:
  // Steps per mm the fixture configures, so a test can convert without guessing.
  static constexpr float STEPS_PER_MM = 80.0f;

  SimulatedMachine() {
    was_connected = MYSERIAL1.host_connected;
    MYSERIAL1.host_connected = false;          // no host: reports must not fill the buffer
    was_state = marlin.state;
    marlin.setState(MF_RUNNING);               // motion is ignored while not running

    SimulatedHardware::ensure_ready();          // main() never runs here
    #ifndef __PLAT_TEST__
      // The LINUX HAL's interrupts are POSIX signals, which would arrive between
      // assertions. Silence them and drive stepper.isr() by hand instead.
      DISABLE_STEPPER_DRIVER_INTERRUPT();
      DISABLE_TEMPERATURE_INTERRUPT();
      HAL_timer_set_compare(MF_TIMER_STEP, HAL_TIMER_TYPE_MAX);
      HAL_timer_set_compare(MF_TIMER_TEMP, HAL_TIMER_TYPE_MAX);
    #endif

    release_kill_button();

    was_settings = planner.settings;
    LOOP_LOGICAL_AXES(i) planner.settings.axis_steps_per_mm[i] = STEPS_PER_MM;
    LOOP_NUM_AXES(i) {
      planner.settings.max_feedrate_mm_s[i] = 300.0f;
      planner.settings.max_acceleration_mm_per_s2[i] = 3000;
    }
    planner.settings.acceleration = 3000;
    planner.settings.travel_acceleration = 3000;
    planner.settings.min_feedrate_mm_s = 0;
    planner.settings.min_travel_feedrate_mm_s = 0;
    planner.refresh_positioning();
    planner.refresh_acceleration_rates();

    planner.clear_block_buffer();
    was_position = motion.position;
    motion.set_all_homed();                    // limits and moves need a known position
  }

  ~SimulatedMachine() {
    planner.clear_block_buffer();
    planner.settings = was_settings;
    planner.refresh_positioning();
    planner.refresh_acceleration_rates();
    motion.position = was_position;
    marlin.setState(was_state);
    MYSERIAL1.host_connected = was_connected;
  }

  // See SimulatedHardware: the button reads "held" from reset, and waiting reaches it.
  static void release_kill_button() { SimulatedHardware::release_kill_button(); }

  // Let the machine move until the planner is empty.
  // Returns false if it did not finish, so a stuck queue fails rather than hangs.
  //
  // Under the test HAL the machine moves because time moves: advancing the clock runs
  // whichever step interrupts fall inside the interval. Calling stepper.isr() by hand
  // here would step the motors while the clock stood still — the LINUX build has to do
  // that only because its interrupts are real signals, which a test cannot schedule.
  static bool run_until_idle(const uint32_t max_steps = 20000000) {
    #ifdef __PLAT_TEST__
      constexpr uint32_t SLICE_US = 100;
      for (uint32_t i = 0; i < max_steps / SLICE_US; i++) {
        if (!planner.has_blocks_queued()) return true;
        HAL_test_advance_micros(SLICE_US);
      }
    #else
      for (uint32_t i = 0; i < max_steps; i++) {
        if (!planner.has_blocks_queued()) return true;
        stepper.isr();
      }
    #endif
    return false;
  }

  // Run the interrupt a fixed number of times, for tests that want partial progress.
  static void step(const uint32_t times) {
    #ifdef __PLAT_TEST__
      for (uint32_t i = 0; i < times; i++) HAL_test_advance_micros(100);
    #else
      for (uint32_t i = 0; i < times; i++) stepper.isr();
    #endif
  }

  // Where the steppers actually are, in millimetres.
  static float stepper_mm(const AxisEnum axis) {
    return float(stepper.position(axis)) / STEPS_PER_MM;
  }

private:
  bool was_connected;
  MarlinState was_state;
  planner_settings_t was_settings;
  xyze_pos_t was_position;
};
