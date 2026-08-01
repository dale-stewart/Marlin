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
  SerialCapture() {
    was_connected = MYSERIAL1.host_connected;
    MYSERIAL1.host_connected = true;
    while (MYSERIAL1.transmit_buffer.available()) (void)MYSERIAL1.transmit_buffer.read();
    running = true;
    drainer = std::thread([this] {
      while (running.load()) {
        drain();
        std::this_thread::yield();
      }
      drain();
    });
  }

  ~SerialCapture() { finish(); MYSERIAL1.host_connected = was_connected; }

  // Stop draining and return everything the firmware wrote.
  const std::string& finish() {
    if (running.exchange(false)) drainer.join();
    return text;
  }

  // Did the output contain this substring?
  bool saw(const char * const needle) { return finish().find(needle) != std::string::npos; }

private:
  void drain() {
    while (MYSERIAL1.transmit_buffer.available())
      text += char(MYSERIAL1.transmit_buffer.read());
  }

  std::string text;
  std::atomic<bool> running{false};
  std::thread drainer;
  bool was_connected = true;
};
