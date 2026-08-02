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
 * and the real stepper ISR steps the real planner. So this fixture initialises the
 * hardware, immediately masks the signals so nothing fires behind the test's back, and
 * lets the test say when time passes.
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
#ifdef __PLAT_TEST__
  #include "src/HAL/TEST/timers.h"
#else
  #include "src/HAL/LINUX/timers.h"
#endif

class SimulatedMachine {
public:
  // Steps per mm the fixture configures, so a test can convert without guessing.
  static constexpr float STEPS_PER_MM = 80.0f;

  SimulatedMachine() {
    was_connected = MYSERIAL1.host_connected;
    MYSERIAL1.host_connected = false;          // no host: reports must not fill the buffer
    was_state = marlin.state;
    marlin.setState(MF_RUNNING);               // motion is ignored while not running

    if (!hardware_ready) {
      HAL_timer_init();                        // main() never runs here
      stepper.init();
      hardware_ready = true;
      #ifdef __PLAT_TEST__
        // Under the test HAL an interrupt fires because time crossed the timer's
        // compare value, so the timer has to be armed and enabled. Driving the ISR
        // directly, as the LINUX build does, needs neither.
        HAL_timer_start(MF_TIMER_STEP, STEPPER_TIMER_RATE / 1000);
        ENABLE_STEPPER_DRIVER_INTERRUPT();
      #endif
    }
    // Nothing may fire on its own: the test decides when the machine moves.
    //
    // Masking the signal is not enough. HAL_timer_init sets up POSIX interval timers,
    // and stepper.init() arms one; a masked signal is still *queued*, so the moment
    // anything re-enables interrupts the backlog is delivered and the ISR runs behind
    // the test's back. Pushing the compare value far into the future disarms them, so
    // the only way the machine moves is a test calling stepper.isr().
    DISABLE_STEPPER_DRIVER_INTERRUPT();
    DISABLE_TEMPERATURE_INTERRUPT();
    HAL_timer_set_compare(MF_TIMER_STEP, HAL_TIMER_TYPE_MAX);
    HAL_timer_set_compare(MF_TIMER_TEMP, HAL_TIMER_TYPE_MAX);

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

  // Let the machine move: run the stepper interrupt until the planner is empty.
  // Returns false if it did not finish, so a stuck queue fails rather than hangs.
  static bool run_until_idle(const uint32_t max_steps = 20000000) {
    for (uint32_t i = 0; i < max_steps; i++) {
      if (!planner.has_blocks_queued()) return true;
      stepper.isr();
    }
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
  static inline bool hardware_ready = false;   // init the HAL once per process
  bool was_connected;
  MarlinState was_state;
  planner_settings_t was_settings;
  xyze_pos_t was_position;
};
