/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
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

#include "../../inc/MarlinConfigPre.h"

#include <iostream>
#include <stdint.h>
#include <stdarg.h>

#ifdef HAS_LIBBSD
  #include <bsd/string.h>
#endif

#undef min
#undef max
#include <algorithm>

#include "hardware/Clock.h"

// Advancing simulated time lives with the timers; idletask() below uses it.
void HAL_test_advance_micros(const uint32_t us);

// What a poll of an empty port costs in simulated time. Zero — free — unless a test says
// otherwise, which is how a busy-wait bounded by elapsed time is made to terminate at all.
// See HAL.cpp for the reasoning and the two caveats.
void HAL_test_set_idle_poll_nanos(const uint64_t ns);
#include "../shared/Marduino.h"
#include "../shared/math_32bit.h"
#include "../shared/HAL_SPI.h"
#include "fastio.h"
#include "serial.h"

// ------------------------
// Defines
// ------------------------

#define CPU_32_BIT
#define SHARED_SERVOS HAS_SERVOS  // Use shared/servos.cpp

#define F_CPU 100000000UL
#define SystemCoreClock F_CPU

#define DELAY_CYCLES(x) Clock::delayCycles(x)

#define CPU_ST7920_DELAY_1 600
#define CPU_ST7920_DELAY_2 750
#define CPU_ST7920_DELAY_3 750

void _printf(const  char *format, ...);
void _putc(uint8_t c);
uint8_t _getc();

//arduino: Print.h
#define DEC 10
#define HEX 16
#define OCT  8
#define BIN  2
//arduino: binary.h (weird defines)
#define B01 1
#define B10 2

// ------------------------
// Serial ports
// ------------------------

extern MSerialT usb_serial;
#define MYSERIAL1 usb_serial

/**
 * A second port, for a display that talks over serial rather than through a UI callback.
 *
 * The DWIN panels are driven by writing bytes at a screen, not by calling an interface, so
 * there is nothing to record the way `stub_extui` records ExtUI calls — the only observable
 * is the byte stream. Giving the test HAL a real second port makes that stream readable, so
 * a test can assert what the firmware told the display rather than only that it did not
 * crash while telling it.
 *
 * `shared/serial_ports.h` wants `MSERIAL(n)`; this HAL has no numbered-port machinery and a
 * second named instance is the smaller thing to provide.
 */
extern MSerialT lcd_serial;
#define LCD_SERIAL lcd_serial

//
// Interrupts
//
#define CRITICAL_SECTION_START()
#define CRITICAL_SECTION_END()

// ADC
#define HAL_ADC_VREF_MV   5000
#define HAL_ADC_RESOLUTION  10

// ------------------------
// Class Utilities
// ------------------------

#pragma GCC diagnostic push
#if GCC_VERSION <= 50000
  #pragma GCC diagnostic ignored "-Wunused-function"
#endif

int freeMemory();

#pragma GCC diagnostic pop

// ------------------------
// MarlinHAL Class
// ------------------------

class MarlinHAL {
public:

  // Earliest possible init, before setup()
  MarlinHAL() {}

  // Watchdog
  static void watchdog_init() {}
  static void watchdog_refresh() {}

  static void init() {}        // Called early in setup()
  static void init_board() {}  // Called less early in setup()
  static void reboot();        // Reset the application state and GPIO

  // Interrupts
  static bool isr_state() { return true; }
  static void isr_on()  {}
  static void isr_off() {}

  static void delay_ms(const int ms) { delay(ms); }

  // Tasks, called from marlin.idle()
  /**
   * Waiting costs time here, as it does on hardware.
   *
   * Marlin waits by spinning on idle(): `planner.synchronize()` until the queue drains,
   * `dwell()` until a period elapses, homing until an endstop trips. On a real board
   * each pass through idle() burns real microseconds and interrupts fire meanwhile, so
   * the condition eventually changes. Under a clock that only moves when asked, an idle
   * task that did nothing would spin forever — the wait would be the one thing that
   * could never end.
   *
   * So advancing the clock here is not a convenience for tests; it is what makes this
   * HAL's idle() mean the same thing as every other HAL's. The step is small enough
   * that a wait ends close to when it should, and advancing runs whichever timer
   * interrupts fall inside it, which is how the queue drains while a command waits.
   */
  static void idletask() { HAL_test_advance_micros(IDLE_STEP_US); }

  static constexpr uint32_t IDLE_STEP_US = 100;

  // Reset
  static constexpr uint8_t reset_reason = RST_POWER_ON;
  static uint8_t get_reset_source() { return reset_reason; }
  static void clear_reset_source() {}

  // Free SRAM
  static int freeMemory() { return ::freeMemory(); }

  //
  // ADC Methods
  //

  static uint8_t active_ch;

  // Called by Temperature::init once at startup
  static void adc_init() {}

  // Called by Temperature::init for each sensor at startup
  static void adc_enable(const uint8_t) {}

  // Begin ADC sampling on the given channel
  static void adc_start(const uint8_t ch) { active_ch = ch; }

  // Is the ADC ready for reading?
  static bool adc_ready() { return true; }

  // The current value of the ADC register
  static uint16_t adc_value();

  /**
   * Set the PWM duty cycle for the pin to the given value.
   * No option to change the resolution or invert the duty cycle.
   */
  static void set_pwm_duty(const pin_t pin, const uint16_t v, const uint16_t=255, const bool=false) {
    analogWrite(pin, v);
  }

  static void set_pwm_frequency(const pin_t, int) {}

  #ifndef HAS_LIBBSD
    /**
     * Redirect missing strlcpy here
     */
    static size_t _strlcpy(char *dst, const char *src, size_t dsize);
    #define strlcpy hal._strlcpy
  #endif

};
