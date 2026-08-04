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
 * A bed that is not quite flat, and a probe that can tell.
 *
 * `SimulatedAxisWithLimit` models a switch that closes at one fixed place, which is what a
 * limit switch is. A probe is the same mechanism asking a different question: it closes
 * when the nozzle reaches *the bed*, and the whole point of levelling is that the bed is at
 * a different height depending on where over it you are. So the trip point cannot be a
 * constant — it has to be a function of the carriage's X and Y.
 *
 * The surface is a plane, `z = height_at_origin + tilt_x * x + tilt_y * y`, because that is
 * exactly what the planar fit under test is able to represent. Giving the fit a surface it
 * cannot represent would test the residual error of a least-squares fit rather than the
 * levelling, and would leave every assertion with a tolerance chosen to make it pass.
 *
 * Stating the surface as a formula rather than as a table of measurements is what makes the
 * tests derivable: the height at any probe point is known in advance from the plane, so a
 * measurement can be checked against the surface instead of against a previous run.
 *
 * X and Y come from the simulated carriages rather than from `stepper.position()`, for the
 * reason given in `simulated_endstops.h` — homing resets the firmware's step counters
 * part-way through a sequence, so a surface driven from them would move under the probe.
 */

#include "src/inc/MarlinConfig.h"
#include "simulated_endstops.h"

#include <math.h>

#ifdef __PLAT_TEST__

class SimulatedBed : public Peripheral {
public:

  /**
   * @param x, y              the carriages whose position the surface height depends on
   * @param steps_per_mm      the resolution shared by all three axes in this fixture
   * @param height_at_origin  bed height at X0 Y0, in mm — where the surface sits
   * @param tilt_x, tilt_y    slope in mm of height per mm of travel, so 0.001 is 0.1 mm
   *                          across a 100 mm bed: a real machine's worth of error
   * @param start_z_mm        where the nozzle starts, in mm above the origin
   */
  SimulatedBed(const SimulatedAxisWithLimit &x, const SimulatedAxisWithLimit &y,
               const float steps_per_mm,
               const float height_at_origin, const float tilt_x, const float tilt_y,
               const float start_z_mm)
    : x_axis(x), y_axis(y), spm(steps_per_mm),
      height_at_origin(height_at_origin), tilt_x(tilt_x), tilt_y(tilt_y),
      carriage_steps(int32_t(start_z_mm * steps_per_mm)) {
    Gpio::attachPeripheral(Z_STEP_PIN, this);
    settle();
  }

  ~SimulatedBed() {
    Gpio::attachPeripheral(Z_STEP_PIN, nullptr);
    // Leave the probe open, or the next test starts with it already pressed.
    Gpio::set(Z_MIN_PIN, Z_MIN_ENDSTOP_HIT_STATE ? 0 : 1);
  }

  void update() override {}

  void interrupt(GpioEvent ev) override {
    if (ev.pin_id != Z_STEP_PIN || ev.event != GpioEvent::RISE) return;
    const bool dir_high = Gpio::get(Z_DIR_PIN) != 0;
    carriage_steps += (dir_high != ENABLED(INVERT_Z_DIR)) ? +1 : -1;
    settle();
  }

  // The height of the surface under a given point — the formula the tests assert against.
  float height_at(const float x_mm, const float y_mm) const {
    return height_at_origin + tilt_x * x_mm + tilt_y * y_mm;
  }

  // ...and under wherever the carriage is now.
  float height_here() const { return height_at(x_mm(), y_mm()); }

  float nozzle_mm() const { return float(carriage_steps) / spm; }
  bool touching() const { return nozzle_mm() <= height_here(); }

  // Put the nozzle somewhere without stepping there. Rounded, not truncated: truncation
  // is toward zero, so it would bias upward below the origin and downward above it, and a
  // fixture that quietly moves the nozzle half a step is a fixture that decides its own
  // boundary tests.
  void place_nozzle_at(const float z_mm) {
    carriage_steps = int32_t(lroundf(z_mm * spm));
    settle();
  }

  // One step, in mm. A test asking whether the probe is open *just* above the surface has
  // to ask from at least this far away — the carriage cannot be anywhere else.
  float step_mm() const { return 1.0f / spm; }

private:

  float x_mm() const { return float(x_axis.position()) / spm; }
  float y_mm() const { return float(y_axis.position()) / spm; }

  void settle() {
    const bool shut = touching();
    Gpio::set(Z_MIN_PIN, uint16_t(shut == (Z_MIN_ENDSTOP_HIT_STATE != 0) ? 1 : 0));
  }

  const SimulatedAxisWithLimit &x_axis, &y_axis;
  const float spm, height_at_origin, tilt_x, tilt_y;
  int32_t carriage_steps;
};

#endif // __PLAT_TEST__
