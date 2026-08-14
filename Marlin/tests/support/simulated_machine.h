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

    release_kill_button();

    /**
     * Every axis, extruders included — and the loop macro is the whole point.
     *
     * This used to set steps-per-millimetre with `LOOP_LOGICAL_AXES` and the limits with
     * `LOOP_NUM_AXES`, which differ by exactly one thing: `NUM_AXES` excludes E. So the extruder
     * was given a resolution and **no maximum feedrate or acceleration at all** — and
     * `planner.settings` is a zero-initialised static, so what it kept was zero.
     *
     * A ceiling of zero does not refuse the move. The planner scales the whole move down until
     * its worst axis is inside its limit, so the block is queued, the stepper takes exactly the
     * right number of steps, and the machine arrives at the right place — at about
     * 0.00007 mm/s. Measured: a 2 mm retract cost **27,487,964 ms of simulated time**, seven and
     * a half hours, while the identical 2 mm move on X through the same call took 152 ms.
     *
     * Nothing failed. The suite got slower, and a test elsewhere that dwells for "the clock so
     * far" began dwelling for forty-five simulated hours and timed out — which is where it
     * finally surfaced, in a file that has nothing to do with extrusion. This is why the
     * "leaked scale" note in `quiesce_simulated_peripherals()` warns that the symptom of a wrong
     * scale is a suite that stops finishing rather than one that fails.
     *
     * `LOOP_DISTINCT_AXES` is the right one for all three arrays: it covers the per-extruder
     * entries as well, which `LOOP_LOGICAL_AXES` does not when `DISTINCT_E_FACTORS` is on.
     */
    was_settings = planner.settings;
    LOOP_DISTINCT_AXES(i) {
      planner.settings.axis_steps_per_mm[i] = STEPS_PER_MM;
      planner.settings.max_feedrate_mm_s[i] = 300.0f;
      planner.settings.max_acceleration_mm_per_s2[i] = 3000;
    }
    /**
     * Three accelerations, and a move uses exactly one of them.
     *
     * `planner.cpp` picks per move: `travel_acceleration` when nothing is extruded,
     * `retract_acceleration` when *only* the extruder moves, and `acceleration` otherwise. This
     * fixture used to set two of the three, so an E-only move — a retract, an unretract, a purge,
     * everything the filament-change feature is made of — was planned with an acceleration of
     * **zero**, because `planner.settings` is a zero-initialised static.
     *
     * Zero acceleration does not refuse the move either. It plans it at about one step per
     * second: a 2 mm retract took **171,932 ms of simulated time**, and the *same* 2 mm of
     * extrusion combined with a 2 mm X move took 152 ms — because adding an XYZ component
     * changes which of the three constants the planner reaches for. That difference is what
     * finally identified it, after the distance-scaling test showed the cost was a fixed wait
     * rather than a slow move.
     */
    planner.settings.acceleration = 3000;
    planner.settings.travel_acceleration = 3000;
    planner.settings.retract_acceleration = 3000;
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
  // The machine moves because time moves: advancing the clock runs whichever step
  // interrupts fall inside the interval. Calling stepper.isr() by hand would step the
  // motors while the clock stood still, which is what a test build had to do when the
  // suite could also be built against real signals it could not schedule.
  static bool run_until_idle(const uint32_t max_steps = 20000000) {
    constexpr uint32_t SLICE_US = 100;
    for (uint32_t i = 0; i < max_steps / SLICE_US; i++) {
      if (!planner.has_blocks_queued()) return true;
      HAL_test_advance_micros(SLICE_US);
    }
    return false;
  }

  // Let a fixed amount of time pass, for tests that want partial progress.
  static void step(const uint32_t times) {
    for (uint32_t i = 0; i < times; i++) HAL_test_advance_micros(100);
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
