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
 * Somebody walks over and presses the kill button.
 *
 * `Marlin::kill()` is the machine's emergency stop, and it was recorded here for a long time
 * as code with no observable outcome — "`minkill()` ends in `for (;;) hal.watchdog_refresh()`
 * and never returns", which put the thermal-error paths, `M112` and the autotune watchdog all
 * behind an unreachable wall. That reading was of the wrong arm. `minkill()` is:
 *
 *     #if ANY(HAS_KILL, SOFT_RESET_ON_KILL)
 *         while (kill_state())  hal.watchdog_refresh();   // wait for release
 *         while (!kill_state()) hal.watchdog_refresh();   // wait for a press
 *         hal.reboot();
 *     #else
 *         for (;;) hal.watchdog_refresh();
 *     #endif
 *
 * This board has a `KILL_PIN`, so it compiles the first arm, and `MarlinHAL::reboot()` under
 * the test HAL has an empty body. **`kill()` returns** — once an operator has released the
 * button and pressed it again, which is exactly what the firmware is waiting for. The original
 * observation that a probe "hangs the binary" was accurate; the explanation was not. Nobody was
 * pressing the button.
 *
 * So this fixture is the operator. Two things about it are load-bearing:
 *
 * **It has to be another thread.** Neither loop advances simulated time or returns to the
 * caller, so nothing the test body does after `kill()` can ever run. This is the same
 * arrangement `SerialCapture` uses for its drainer and `M0` uses for its answer, and it shares
 * the same caveat: the delay is wall-clock, because simulated time only moves when the machine
 * moves it, and the machine is busy waiting.
 *
 * **It presses and then releases.** Unity's failure path is a `longjmp` that skips destructors,
 * so a fixture that left the button held would hand the next test a machine whose
 * `manage_inactivity()` calls `kill()` on its 250th idle — and *that* kill would hang in the
 * first loop, waiting for a release nobody is going to perform. Ending released costs one more
 * line and removes the whole failure mode. `quiesce_simulated_peripherals()` releases it too,
 * for the case where this object is destroyed before its thread has run.
 */

#include "src/inc/MarlinConfig.h"
#include "src/MarlinCore.h"
#include "simulated_hardware.h"

#include <thread>
#include <chrono>
#include <atomic>

#if HAS_KILL

class OperatorPressesKill {
public:

  /**
   * @param press_after_ms   how long to wait before pressing — long enough that the machine
   *                         has reached the second loop, short enough not to dominate the run
   * @param hold_for_ms      how long to hold it down, generously wide so that a slow machine
   *                         cannot miss the window
   */
  explicit OperatorPressesKill(const uint32_t press_after_ms = 20, const uint32_t hold_for_ms = 400) {
    SimulatedHardware::release_kill_button();   // so the "wait for release" loop exits at once

    presser = std::thread([press_after_ms, hold_for_ms] {
      std::this_thread::sleep_for(std::chrono::milliseconds(press_after_ms));
      WRITE(KILL_PIN, KILL_PIN_STATE);
      std::this_thread::sleep_for(std::chrono::milliseconds(hold_for_ms));
      WRITE(KILL_PIN, !KILL_PIN_STATE);
    });
  }

  /**
   * Start with the button already held, so the machine has to see it released *before* it will
   * accept the press. That is the sequence `minkill()` insists on, and a machine that skipped
   * the first loop would reboot the instant it was killed by someone still holding the button.
   */
  static OperatorPressesKill already_holding_it() {
    has_let_go = false;
    WRITE(KILL_PIN, KILL_PIN_STATE);
    return OperatorPressesKill(HeldFirst{});
  }

  /**
   * Has the operator let go yet?
   *
   * The discriminator for the release-then-press sequence, and deliberately not a wall-clock
   * measurement. `kill()` returns while the button is still *held* — it stops at the press,
   * not at the next release — so nothing about the pin state afterwards says whether the
   * release was ever seen. This flag is set by the operator between letting go and pressing
   * again, so firmware that accepted the held button would return before it was ever true.
   */
  static bool the_operator_let_go_first() { return has_let_go.load(); }

  ~OperatorPressesKill() {
    if (presser.joinable()) presser.join();
    SimulatedHardware::release_kill_button();
  }

  OperatorPressesKill(OperatorPressesKill &&other) noexcept : presser(std::move(other.presser)) {}

private:

  struct HeldFirst {};

  explicit OperatorPressesKill(HeldFirst) {
    presser = std::thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      WRITE(KILL_PIN, !KILL_PIN_STATE);         // let go...
      has_let_go = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      WRITE(KILL_PIN, KILL_PIN_STATE);          // ...and press again
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      WRITE(KILL_PIN, !KILL_PIN_STATE);
    });
  }

  static inline std::atomic<bool> has_let_go{false};
  std::thread presser;
};

#endif // HAS_KILL
