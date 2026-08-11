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
 * An I2C bus with nothing attached to it.
 *
 * Arduino's `Wire` does not exist on the host, so anything that talks I2C — the position
 * encoders, some EEPROMs, some displays — cannot be built for a test at all. These symbols
 * are what make it link, on the same footing as `HAL/TEST/spi.cpp`: a bus, not a device.
 *
 * The behaviour modelled is an idle bus. Nothing acknowledges, `requestFrom()` yields no
 * bytes, and `read()` returns 0xFF because both lines are pulled up and no device is driving
 * them. That makes "there is no device at that address" the default answer, which is the
 * honest one and the state a board is in with nothing plugged into the header.
 *
 * A test that wants a device answering should attach one rather than teach this class to
 * pretend. The pattern for that is `tests/support/simulated_endstops.h` and its relatives:
 * a peripheral with its own state, attached to the bus, whose replies are derived from the
 * simulated machine rather than recorded. Faking the replies here instead would mean encoding
 * one device's register map into the bus itself, which is the wrong layer and only ever
 * describes one device.
 */

#ifdef __PLAT_TEST__

#include <stdint.h>
#include <stddef.h>

class TestTwoWire {
public:
  void begin() {}
  void begin(uint8_t) {}
  void end() {}
  void setClock(uint32_t) {}

  void beginTransmission(uint8_t address) { addressed = address; }

  // 2 = "address send, NACK received", which is what a bus with nothing on it reports.
  uint8_t endTransmission(uint8_t = 1) { return 2; }

  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t *, size_t n) { return n; }

  uint8_t requestFrom(uint8_t, uint8_t, uint8_t = 1) { return 0; }   // nobody answered

  int available() { return 0; }
  int read() { return 0xFF; }   // SDA idles high: no device is driving the line
  int peek() { return 0xFF; }

private:
  uint8_t addressed = 0;
};

extern TestTwoWire Wire;

#endif // __PLAT_TEST__
