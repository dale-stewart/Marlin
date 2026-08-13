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
 * A magnetic position encoder on the I2C bus.
 *
 * The point of closed-loop feedback is that the encoder measures the *carriage*, not the
 * firmware's opinion of it. So this reads its count from a `SimulatedAxisWithLimit` — the same
 * thing the step pulses drive — for the reason given in `simulated_endstops.h`: anything derived
 * from `stepper.position()` moves when the firmware re-references its own counters, and homing
 * and tool changes do exactly that. An encoder that followed the firmware's belief could never
 * disagree with it, which is the one thing an encoder is for.
 *
 * The wire format is three bytes, little-endian, from `get_raw_count()`:
 *
 *   bits 0..21   the count, two's complement
 *   bit  21      the sign, which the firmware sign-extends into the top bits
 *   bits 22..23  magnetic field strength: 0 good, 1 fair, 2 bad
 *
 * `ticks_per_mm` is the encoder's own resolution and has nothing to do with the machine's steps
 * per millimetre. Keeping the two different in a test is what makes a calibration that confuses
 * them visible.
 */

#include "src/inc/MarlinConfig.h"

#if ENABLED(I2C_POSITION_ENCODERS)

#include "src/HAL/TEST/include/Wire.h"
#include "simulated_endstops.h"

class SimulatedI2CEncoder : public I2CDevice {
public:

  /**
   * @param address       where it answers on the bus
   * @param axis          the carriage it is measuring
   * @param steps_per_mm  the machine's resolution, to turn that carriage's steps into millimetres
   * @param ticks_per_mm  the encoder's own resolution
   */
  SimulatedI2CEncoder(const uint8_t address, const SimulatedAxisWithLimit &axis,
                      const float steps_per_mm, const float ticks_per_mm)
    : address(address), axis(axis), spm(steps_per_mm), tpm(ticks_per_mm) {
    Wire.attach(address, this);
  }

  ~SimulatedI2CEncoder() { Wire.attach(address, nullptr); }

  // Where the encoder says the carriage is, in millimetres — the reading a test predicts
  // independently of anything the firmware computed.
  float mm() const { return float(axis.position()) / spm - zero_mm; }

  // Call it home: the count is measured from wherever the carriage is now.
  void zero_here() { zero_mm = float(axis.position()) / spm; }

  // A strip that has come loose, or a head too far from it. The firmware should notice.
  void report_field_strength(const uint8_t h) { field = h; }

  void respond(uint8_t *buffer, const uint8_t count) override {
    const int32_t ticks = int32_t(mm() * tpm);
    const uint32_t packed = (uint32_t(ticks) & 0x003FFFFF) | (uint32_t(field & 0x03) << 22);
    for (uint8_t i = 0; i < count; i++) buffer[i] = uint8_t((packed >> (8 * i)) & 0xFF);
  }

private:
  const uint8_t address;
  const SimulatedAxisWithLimit &axis;
  const float spm, tpm;
  float zero_mm = 0;
  uint8_t field = 0;                // I2CPE_MAG_SIG_GOOD
};

#endif // ENABLED(I2C_POSITION_ENCODERS)
