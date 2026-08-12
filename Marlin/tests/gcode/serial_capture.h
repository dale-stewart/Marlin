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
 * Capture what a command reports back to the host.
 *
 * The native HAL's serial write busy-waits for room in a 128-byte transmit buffer, and
 * nothing drains it outside the simulator — so a report longer than the buffer would
 * spin forever. This drains it from a second thread for as long as the capture is in
 * scope, which is what the simulator's UI does, and collects the bytes so a test can
 * assert on what the host would have seen.
 *
 * The buffer is a single-producer single-consumer ring; the firmware writes, the
 * drainer reads, and the collected text is only touched after the thread is joined.
 */

#include "src/inc/MarlinConfig.h"
#include <string>
#include <thread>
#include <atomic>

class SerialCapture {
public:
  /**
   * Defaults to the host port. Pass another to capture it instead — `LCD_SERIAL` for a
   * display that is driven by writing bytes at it rather than through a UI interface,
   * where the byte stream is the only observable there is.
   */
  SerialCapture(MSerialT &p = MYSERIAL1) : port(p) {
    was_connected = port.host_connected;
    port.host_connected = true;
    while (port.transmit_buffer.available()) (void)port.transmit_buffer.read();
    running = true;
    drainer = std::thread([this] {
      while (running.load()) {
        drain();
        std::this_thread::yield();
      }
      drain();
    });
  }

  ~SerialCapture() { finish(); port.host_connected = was_connected; }

  // Stop draining and return everything the firmware wrote.
  //
  // The final drain runs on the calling thread after the drainer has been joined:
  // bytes can be written between the drainer's last read and its exit, and collecting
  // them here rather than racing for them is what keeps the result deterministic.
  const std::string& finish() {
    if (running.exchange(false)) drainer.join();
    drain();
    return text;
  }

  // Did the output contain this substring?
  bool saw(const char * const needle) { return finish().find(needle) != std::string::npos; }

private:
  void drain() {
    while (port.transmit_buffer.available())
      text += char(port.transmit_buffer.read());
  }

  MSerialT &port;
  std::string text;
  std::atomic<bool> running{false};
  std::thread drainer;
  bool was_connected = true;
};
