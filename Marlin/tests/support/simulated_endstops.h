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
 * A carriage on a rail, with a limit switch at one end of it.
 *
 * Homing cannot be tested by writing to the endstop pin from the test body, because
 * `G28` never returns to the test body until it has finished: it spins inside
 * `planner.synchronize()`. The switch has to close *while* the firmware is moving, for
 * the same reason it does on a machine — because the carriage arrived at it.
 *
 * `Gpio::attachPeripheral()` is the hook for exactly that. Every write to a pin calls the
 * attached peripheral, so watching the step pin gives a pulse-accurate model:
 *
 *   - each rising edge on STEP moves the carriage one step, in whichever direction the
 *     DIR pin is asserting (through the axis's INVERT_*_DIR, the way a driver reads it);
 *   - the switch is closed whenever the carriage is at or past the trip point;
 *   - the pin is driven to the configured `*_ENDSTOP_HIT_STATE` when closed, and away
 *     from it when not.
 *
 * The carriage's position is the test's own bookkeeping, deliberately *not* read back
 * from `stepper.position()`. Homing resets the stepper count to zero at the start of
 * every homing move (`do_homing_move()` does `set_machine_position_mm()` before it
 * buffers), so the firmware's idea of where the axis is jumps twice during one `G28`.
 * A switch driven from that count would trip in a different physical place each time and
 * could not tell a correct homing sequence from an incorrect one.
 *
 * `HAL/TEST/hardware/LinearAxis` models the same thing for the simulator, but it seeds
 * its position from `rand()` and its destructor does not detach the peripheral, so a test
 * using it would be non-repeatable and would leave a dangling pointer for the next test.
 */

#include "src/inc/MarlinConfig.h"
#include "src/module/stepper.h"

#ifdef __PLAT_TEST__
  #include "src/HAL/TEST/hardware/Gpio.h"
#else
  #include "src/HAL/LINUX/hardware/Gpio.h"
#endif

class SimulatedAxisWithLimit : public Peripheral {
public:

  /**
   * @param step_pin      the axis's STEP pin, watched for pulses
   * @param dir_pin       the axis's DIR pin, read at each pulse
   * @param dir_inverted  the axis's INVERT_*_DIR, so a DIR level means the same thing
   *                      here as it does at the driver
   * @param limit_pin     the limit switch's pin
   * @param hit_state     the level that pin takes when the switch is closed
   * @param trip_steps    where the switch closes, in steps from the origin
   * @param start_steps   where the carriage starts, in steps from the origin
   */
  SimulatedAxisWithLimit(const pin_t step_pin, const pin_t dir_pin, const bool dir_inverted,
                         const pin_t limit_pin, const uint8_t hit_state,
                         const int32_t trip_steps, const int32_t start_steps)
    : step_pin(step_pin), dir_pin(dir_pin), dir_inverted(dir_inverted),
      limit_pin(limit_pin), hit_state(hit_state),
      trip_steps(trip_steps), carriage_steps(start_steps),
      lowest_steps(start_steps), highest_steps(start_steps) {
    Gpio::attachPeripheral(step_pin, this);
    settle();
  }

  ~SimulatedAxisWithLimit() {
    Gpio::attachPeripheral(step_pin, nullptr);
    // Leave the switch open, or the next test starts against a pressed limit.
    Gpio::set(limit_pin, hit_state ? 0 : 1);
  }

  void update() override {}

  void interrupt(GpioEvent ev) override {
    if (ev.pin_id != step_pin || ev.event != GpioEvent::RISE) return;
    // A driver steps in the direction DIR asserts, after the axis's own inversion.
    const bool dir_high = Gpio::get(dir_pin) != 0;
    carriage_steps += (dir_high != dir_inverted) ? +1 : -1;
    if (carriage_steps < lowest_steps) lowest_steps = carriage_steps;
    if (carriage_steps > highest_steps) highest_steps = carriage_steps;
    settle();
  }

  // Where the carriage physically is, in steps and in millimetres.
  int32_t position() const { return carriage_steps; }
  int32_t lowest_reached() const { return lowest_steps; }

  // ...and the far end of the same record. A path that curves away and comes back — an arc, a
  // back-off, a lift — is over by the time it ends, so the final position cannot see how far it
  // went; these two can.
  int32_t highest_reached() const { return highest_steps; }

  // Start the record again from where the carriage is now.
  void forget_extremes() { lowest_steps = highest_steps = carriage_steps; }
  bool closed() const { return carriage_steps <= trip_steps; }

  // How many times the switch went from open to closed — a homing sequence bumps twice.
  uint16_t closures() const { return closure_count; }

  void place_at(const int32_t steps) {
    carriage_steps = lowest_steps = highest_steps = steps;
    settle();
  }

private:

  // Drive the pin to whatever the switch's mechanical state says it should read.
  void settle() {
    const bool shut = closed();
    if (shut && !was_closed) closure_count++;
    was_closed = shut;
    Gpio::set(limit_pin, uint16_t(shut == (hit_state != 0) ? 1 : 0));
  }

  const pin_t step_pin, dir_pin;
  const bool dir_inverted;
  const pin_t limit_pin;
  const uint8_t hit_state;
  const int32_t trip_steps;
  int32_t carriage_steps;
  int32_t lowest_steps, highest_steps;
  bool was_closed = false;
  uint16_t closure_count = 0;
};
