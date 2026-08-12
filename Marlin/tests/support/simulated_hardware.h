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
 * Bring the simulated board up once per test process.
 *
 * `main()` is excluded from a unit test build, so nothing runs `Marlin::setup()` and
 * nothing initialises the hardware. Every fixture that needs a working machine needs
 * the same bring-up, and it must happen exactly once, so it lives here rather than in
 * whichever fixture happened to be constructed first.
 *
 * The order below is the part that matters:
 *
 *  1. Put the pins in the state a board powers up in. Simulated pins all read zero,
 *     which is not a state any board is ever in, and two of those readings are actively
 *     dangerous: LOW on KILL_PIN is "the kill button is held", and zero on a thermistor
 *     input is "the sensor is shorted", which converts to 320 C — above HEATER_0_MAXTEMP
 *     and BED_MAXTEMP. The first `Temperature::task()` after the ISR starts producing
 *     readings would call `kill()`, which never returns.
 *
 *  2. `HAL_timer_init()` before anything that starts a timer. Under the LINUX HAL a
 *     Timer's frequency is zero until `init()` has run, and `Timer::setCompare()`
 *     divides by it — that is the SIGFPE that `Temperature::init()` was recorded as
 *     causing, and it is the same cause that once broke `Stepper::init()`. The test HAL
 *     guards the division, so it survives either way; the LINUX HAL does not.
 *
 *  3. `Temperature::init()` last, because it is what arms and enables MF_TIMER_TEMP.
 *     From here on, advancing simulated time runs the real `Temperature::isr()`.
 */

#include "src/inc/MarlinConfig.h"
#include "src/module/stepper.h"
#include "src/module/temperature.h"
#include "src/MarlinCore.h"

#ifdef __PLAT_TEST__
  #include "src/HAL/TEST/timers.h"
  #include "src/HAL/TEST/hardware/Gpio.h"
#else
  #include "src/HAL/LINUX/timers.h"
  #include "src/HAL/LINUX/hardware/Gpio.h"
#endif

#include <math.h>

class SimulatedHardware {
public:

  // What an idle printer's thermistors report at power-on.
  static constexpr celsius_float_t AMBIENT_C = 25.0f;

  static void ensure_ready() {
    if (ready) return;
    ready = true;

    sane_analog_inputs();
    release_kill_button();
    release_panel_buttons();

    HAL_timer_init();
    stepper.init();

    #ifdef __PLAT_TEST__
      // An interrupt fires here because time crossed a timer's compare value, so each
      // timer has to be armed and enabled or advancing the clock does nothing. The
      // step timer's initial rate is a placeholder: Stepper::isr() programs the real
      // interval for the block it is running, from its first call onward.
      HAL_timer_start(MF_TIMER_STEP, STEPPER_TIMER_RATE / 1000);
      ENABLE_STEPPER_DRIVER_INTERRUPT();

      // Arms and enables MF_TIMER_TEMP, so Temperature::isr() runs from now on.
      thermalManager.init();
    #else
      /**
       * Silence the timers the moment they exist.
       *
       * `Stepper::init()` ends with `HAL_timer_start(MF_TIMER_STEP, 122)` and
       * `wake_up()`, so by the time it returns the LINUX HAL has a live POSIX interval
       * timer delivering SIGRTMIN at 122 Hz — and its handler is the real stepper ISR,
       * arriving between arbitrary instructions of whatever test is running. Masking
       * has to happen here rather than in a fixture's constructor, because the first
       * fixture to want working hardware may not be the one that drives motion.
       */
      DISABLE_STEPPER_DRIVER_INTERRUPT();
      DISABLE_TEMPERATURE_INTERRUPT();
      HAL_timer_set_compare(MF_TIMER_STEP, HAL_TIMER_TYPE_MAX);
      HAL_timer_set_compare(MF_TIMER_TEMP, HAL_TIMER_TYPE_MAX);
    #endif
  }

  /**
   * Say the kill button is not being held down.
   *
   * On a board KILL_PIN is an input with a pull-up, so it reads HIGH — released — from
   * reset, and `Marlin::setup()` configures it that way. Simulated pins all read LOW at
   * reset, and LOW is KILL_PIN_STATE: to the firmware the button is held. Nothing
   * notices until something waits, because `manage_inactivity()` debounces the button
   * over 250 passes before acting — so the 250th call to `marlin.idle()` in the process
   * calls `kill()`, which never returns.
   */
  static void release_kill_button() {
    #if HAS_KILL
      SET_INPUT_PULLUP(KILL_PIN);
      WRITE(KILL_PIN, !KILL_PIN_STATE);
    #endif
  }

  /**
   * Say nobody is touching the knob either.
   *
   * The same fault as the kill button, in a place that costs more. Panel buttons are
   * active-low with pull-ups on a board, and every simulated pin reads LOW at reset, so the
   * firmware in a test build starts up believing the click is held down and both quadrature
   * phases are shorted. That is not a state any encoder can be in.
   *
   * It is quiet rather than fatal, which is what makes it expensive: the suite still passes,
   * because a held button changes what the UI does and not what any assertion looks at. What
   * it changes is how long everything takes — a machine with its knob held gets extra work on
   * every pass through `idle()`, and a suite that ran in eight seconds took ten minutes the
   * first time this board defined encoder pins at all.
   *
   * Released once before the first test, and again after each one, because a test that fails
   * part-way through a click never reaches its own cleanup.
   */
  static void release_panel_buttons() {
    #if BUTTON_EXISTS(ENC)
      SET_INPUT_PULLUP(BTN_ENC);
      WRITE(BTN_ENC, HIGH);
    #endif
    #if BUTTON_EXISTS(EN1)
      SET_INPUT_PULLUP(BTN_EN1);
      WRITE(BTN_EN1, HIGH);
    #endif
    #if BUTTON_EXISTS(EN2)
      SET_INPUT_PULLUP(BTN_EN2);
      WRITE(BTN_EN2, HIGH);
    #endif
  }

  /**
   * The Gpio pin an ADC channel is actually read from.
   *
   * `Temperature::isr()` passes TEMP_0_PIN — an analog *index* — to `hal.adc_start()`,
   * and `MarlinHAL::adc_value()` maps that index onto a digital pin before reading Gpio.
   * A test driving a sensor has to write the same pin the HAL reads, so it goes through
   * the same mapping rather than guessing.
   */
  static pin_t adc_pin(const int8_t channel) { return analogInputToDigitalPin(channel); }

  // Drive one ADC channel to a 10-bit conversion result.
  static void drive_adc(const int8_t channel, const uint16_t code) {
    // MarlinHAL::adc_value() returns (Gpio value >> 2) & 0x3FF.
    Gpio::set(adc_pin(channel), uint16_t((code & 0x3FF) << 2));
  }

  static uint16_t adc_code(const int8_t channel) {
    return uint16_t((Gpio::get(adc_pin(channel)) >> 2) & 0x3FF);
  }

  /**
   * The 10-bit conversion result whose Celsius value is nearest `c`.
   *
   * A thermistor is not linear and the ADC is 10 bits, so not every temperature can be
   * expressed — near 120 C one count is about 0.15 C, near 200 C about 0.45 C. Callers
   * that need to assert on an exact value ask this what they will actually get, rather
   * than asserting a value the hardware could not produce.
   *
   * `conv` takes the accumulated raw reading, which is OVERSAMPLENR conversions summed.
   */
  template <typename Conv>
  static uint16_t code_nearest(Conv conv, const celsius_float_t c) {
    uint16_t best = 0;
    float best_err = 1e30f;
    for (uint16_t code = 0; code < 1024; code++) {
      const float err = fabsf(float(conv(raw_adc_t(code * OVERSAMPLENR))) - float(c));
      if (err < best_err) { best_err = err; best = code; }
    }
    return best;
  }

private:

  // Every analog input starts at the reading an unheated printer would give. See the
  // header comment: zero means "shorted sensor", which is a kill, not a cold printer.
  static void sane_analog_inputs() {
    #if HAS_HOTEND
      const uint16_t room = code_nearest(
        [](const raw_adc_t r) { return thermalManager.analog_to_celsius_hotend(r, 0); }, AMBIENT_C);
    #elif HAS_HEATED_BED
      const uint16_t room = code_nearest(
        [](const raw_adc_t r) { return thermalManager.analog_to_celsius_bed(r); }, AMBIENT_C);
    #else
      const uint16_t room = 512;
    #endif
    for (int8_t ch = 0; ch < NUM_ANALOG_INPUTS; ch++) drive_adc(ch, room);
  }

  static inline bool ready = false;
};
