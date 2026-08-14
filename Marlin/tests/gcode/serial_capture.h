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
 *
 * ## Why the state is on the heap
 *
 * Unity's failure path is a `longjmp`, which unwinds no C++ stack — so when an assertion
 * fails, this object's destructor does not run and its drainer thread is never joined.
 * That thread then keeps appending bytes to a `std::string` that lives in the stack frame
 * the `longjmp` has just discarded, and the frame is reused by whatever runs next. The
 * symptom is a **segmentation fault immediately after the first failing test**, taking the
 * rest of the suite with it: 818 tests become 490, and every failure after the first is
 * invisible because the process is gone.
 *
 * That was worth more than a day of confusion when it surfaced, because it presents as
 * "the suite dies at test N" rather than as anything to do with the test that failed. It
 * is also the half of register #40 that the earlier fix did not cover — marking the port
 * unattached in `quiesce_simulated_peripherals()` stops the *hang*, but nothing stopped
 * the thread, and a thread writing into a dead frame is the more serious of the two.
 *
 * So the drainer owns its state through a `shared_ptr` and never touches `this`. A skipped
 * destructor now leaks a session rather than corrupting memory, and
 * `SerialCapture::release_live_captures()` — called from the quiesce, on the far side of
 * the `longjmp` — stops and joins whatever was left behind. Sessions are held in a list
 * rather than a single slot because captures nest: a test may hold one open while a helper
 * opens another.
 */

#include "src/inc/MarlinConfig.h"
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>

class SerialCapture {
private:

  struct Session {
    MSerialT &port;
    std::string text;
    std::atomic<bool> running{true};
    bool was_connected = true;
    std::thread drainer;
    explicit Session(MSerialT &p) : port(p) {}

    void drain() {
      while (port.transmit_buffer.available())
        text += char(port.transmit_buffer.read());
    }

    // Safe to call more than once: the test's own `finish()` and the quiesce may both run.
    void stop() {
      if (running.exchange(false) && drainer.joinable()) drainer.join();
      drain();
    }
  };

  static std::vector<std::shared_ptr<Session>>& live() {
    static std::vector<std::shared_ptr<Session>> sessions;
    return sessions;
  }

  static void forget(const std::shared_ptr<Session> &s) {
    auto &v = live();
    v.erase(std::remove(v.begin(), v.end(), s), v.end());
  }

public:

  /**
   * Defaults to the host port. Pass another to capture it instead — `LCD_SERIAL` for a
   * display that is driven by writing bytes at it rather than through a UI interface,
   * where the byte stream is the only observable there is.
   */
  SerialCapture(MSerialT &p = MYSERIAL1) : s(std::make_shared<Session>(p)) {
    s->was_connected = p.host_connected;
    p.host_connected = true;
    while (p.transmit_buffer.available()) (void)p.transmit_buffer.read();

    live().push_back(s);

    auto state = s;                       // the thread holds the state alive by itself
    s->drainer = std::thread([state] {
      while (state->running.load()) {
        state->drain();
        std::this_thread::yield();
      }
      state->drain();
    });
  }

  ~SerialCapture() {
    s->stop();
    s->port.host_connected = s->was_connected;
    forget(s);
  }

  // Stop draining and return everything the firmware wrote.
  //
  // The final drain runs on the calling thread after the drainer has been joined:
  // bytes can be written between the drainer's last read and its exit, and collecting
  // them here rather than racing for them is what keeps the result deterministic.
  const std::string& finish() { s->stop(); return s->text; }

  // Did the output contain this substring?
  bool saw(const char * const needle) { return finish().find(needle) != std::string::npos; }

  /**
   * Stop and join any capture whose destructor never ran.
   *
   * Called from `quiesce_simulated_peripherals()`, which runs after the test returns and so
   * on the far side of Unity's `longjmp`. Nothing here touches the abandoned `SerialCapture`
   * object — only its heap-allocated session — because that object's stack frame is gone.
   */
  static void release_live_captures() {
    auto sessions = live();               // by value: stop() below does not mutate this copy
    for (auto &state : sessions) {
      state->stop();
      state->port.host_connected = state->was_connected;
    }
    live().clear();
  }

private:
  std::shared_ptr<Session> s;
};
