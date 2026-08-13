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
 * The knob on the front of the machine.
 *
 * Everything the firmware *sends* a display is observable from a test — the byte stream is
 * right there on the serial port. Nothing a person would have *done to the machine* was, and
 * that is the half of a UI driver where the interesting code lives: a display driver mostly
 * exists to turn gestures into settings.
 *
 * So this stands in for the hand. It drives the two quadrature phases and the click as
 * ordinary pins, which is what they are on the boards these panels ship with, and the
 * firmware reads them through exactly the code it runs on hardware — `MarlinUI` samples
 * `BTN_EN1`/`BTN_EN2` live and debounces them itself.
 *
 * Two things follow from that and shape the interface:
 *
 * - **The firmware, not the fixture, does the sampling.** A phase change is only seen by a
 *   call to the driver, and the delta it produces is consumed by that same call. A fixture
 *   that sampled on its own would eat the input it was meant to deliver. So every motion
 *   here takes the caller's pump — whatever drives the driver, usually `dwinHandleScreen()`.
 *
 * - **A phase change costs two passes and some time.** `MarlinUI::get_encoder_delta()`
 *   debounces: the first pass after an edge only starts the timer, and a later pass past
 *   `ENCODER_DEBOUNCE_MS` commits it. Under this HAL time moves when asked, so the fixture
 *   asks — a fixture that only wrote the pins would produce an encoder nothing ever accepted.
 *
 * Note the pins are active-low and every simulated pin powers up LOW, so an unreleased
 * encoder reads as held from the first instruction — the same trap `KILL_PIN` sets. The
 * constructor releases all three, which is what the board's pull-ups do on hardware.
 */

#include "src/inc/MarlinConfig.h"

#if BUTTON_EXISTS(EN1) && BUTTON_EXISTS(EN2) && BUTTON_EXISTS(ENC)

#include "src/lcd/buttons.h"
#include "test_clock.h"

class SimulatedEncoder {
public:
  // Longer than ENCODER_DEBOUNCE_MS, so a settled edge is committed rather than nearly.
  static constexpr uint32_t SETTLE_MS = 3;

  // A click is ignored unless this long after the last one — see encoderReceiveAnalyze().
  static constexpr uint32_t CLICK_INTERVAL_MS = 301;

  SimulatedEncoder() { release_all(); }
  ~SimulatedEncoder() { release_all(); }

  /**
   * One detent of the knob, which is ENCODER_PULSES_PER_STEP quadrature transitions.
   *
   * The driver accumulates transitions and only reports a direction once a whole detent has
   * arrived, so turning by less than this is a real thing to do and produces nothing — which
   * is why the unit here is the detent and not the pulse.
   */
  template <typename Pump> void turn_clockwise(Pump &&pump, const uint8_t detents = 1) {
    for (uint8_t d = 0; d < detents; ++d)
      for (uint8_t i = 0; i < (ENCODER_PULSES_PER_STEP); ++i) step_phase(+1, pump);
  }

  template <typename Pump> void turn_counterclockwise(Pump &&pump, const uint8_t detents = 1) {
    for (uint8_t d = 0; d < detents; ++d)
      for (uint8_t i = 0; i < (ENCODER_PULSES_PER_STEP); ++i) step_phase(-1, pump);
  }

  /**
   * Press and release the knob.
   *
   * The press is pumped while held, because that is when the driver reads it; the release is
   * pumped too, so a following turn does not start against a button the firmware still
   * believes is down. Time passes afterwards so that a second click is not swallowed by the
   * 300 ms gate the driver applies to repeats.
   */
  template <typename Pump> void click(Pump &&pump) {
    WRITE(BTN_ENC, LOW);
    pump();
    WRITE(BTN_ENC, HIGH);
    pump();
    TestClock::advance_millis(CLICK_INTERVAL_MS);
  }

private:
  // 0:(0,0)  1:(1,0)  2:(1,1)  3:(0,1) — the order MarlinUI decodes as increasing.
  uint8_t phase = 0;

  void release_all() {
    WRITE(BTN_ENC, HIGH);
    write_phase();
  }

  void write_phase() {
    const bool a = phase == 1 || phase == 2,
               b = phase == 2 || phase == 3;
    WRITE(BTN_EN1, a ? LOW : HIGH);   // pressed is low
    WRITE(BTN_EN2, b ? LOW : HIGH);
  }

  template <typename Pump> void step_phase(const int8_t dir, Pump &&pump) {
    phase = uint8_t((phase + dir + 4) & 3);
    write_phase();
    pump();                              // the edge is seen and the debounce timer starts
    TestClock::advance_millis(SETTLE_MS);
    pump();                              // ... and this pass is the one that accepts it
  }
};

#endif // BUTTON_EXISTS(EN1) && BUTTON_EXISTS(EN2) && BUTTON_EXISTS(ENC)
