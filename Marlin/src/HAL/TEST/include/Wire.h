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
 * A test that wants a device answering attaches one, rather than teaching this class to pretend.
 * `I2CDevice` below is the seam for that, and the pattern is the same as `Gpio::attachPeripheral`:
 * the device holds its own state and derives its replies from the simulated machine. Encoding a
 * device's register map into the bus itself would be the wrong layer and would only ever describe
 * one device.
 */

#ifdef __PLAT_TEST__

#include <stdint.h>
#include <stddef.h>

/**
 * Something answering at an address.
 *
 * `respond()` fills the bytes a controller asked for; `receive()` takes the bytes it sent. A
 * device that only ever gets read need not implement the second.
 */
class I2CDevice {
public:
  virtual ~I2CDevice() {}
  virtual void respond(uint8_t *buffer, const uint8_t count) = 0;
  virtual void receive(const uint8_t) {}
};

class TestTwoWire {
public:

  // Attach a device at an address, or pass nullptr to remove one. Attaching in a fixture's
  // constructor and detaching in its destructor matters for the same reason it does for GPIO
  // peripherals: a dangling device is written through after it has been destroyed.
  void attach(const uint8_t address, I2CDevice * const device) {
    for (uint8_t i = 0; i < MAX_DEVICES; i++)
      if (!devices[i] || addresses[i] == address) {
        addresses[i] = address; devices[i] = device;
        return;
      }
  }

  void begin() {}
  void begin(uint8_t) {}
  void end() {}
  void setClock(uint32_t) {}

  void beginTransmission(uint8_t address) { addressed = address; }

  // 0 = acknowledged, 2 = "address sent, NACK received" — which is what an empty bus reports.
  uint8_t endTransmission(uint8_t = 1) { return find(addressed) ? 0 : 2; }

  size_t write(uint8_t b) {
    if (I2CDevice * const d = find(addressed)) d->receive(b);
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) write(b[i]);
    return n;
  }

  uint8_t requestFrom(uint8_t address, uint8_t count, uint8_t = 1) {
    pending = 0; next = 0;
    I2CDevice * const d = find(address);
    if (!d) return 0;                       // nobody answered
    if (count > sizeof(buffer)) count = sizeof(buffer);
    d->respond(buffer, count);
    pending = count;
    return count;
  }

  int available() { return int(pending - next); }
  // SDA idles high, so a read past what was sent is 0xFF rather than a stale byte.
  int read() { return next < pending ? buffer[next++] : 0xFF; }
  int peek() { return next < pending ? buffer[next] : 0xFF; }

private:
  static constexpr uint8_t MAX_DEVICES = 8;
  uint8_t addresses[MAX_DEVICES] = { 0 };
  I2CDevice *devices[MAX_DEVICES] = { nullptr };

  I2CDevice* find(const uint8_t address) {
    for (uint8_t i = 0; i < MAX_DEVICES; i++)
      if (devices[i] && addresses[i] == address) return devices[i];
    return nullptr;
  }

  uint8_t addressed = 0;
  uint8_t buffer[32] = { 0 };
  uint8_t pending = 0, next = 0;
};

extern TestTwoWire Wire;

#endif // __PLAT_TEST__
