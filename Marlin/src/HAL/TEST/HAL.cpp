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

#ifdef __PLAT_TEST__

#include "../../inc/MarlinConfig.h"
#include "../shared/Delay.h"

/**
 * Making a busy-wait terminate.
 *
 * Time here advances only when a test asks, which is what makes every wait deterministic — and
 * it has one blind spot. Firmware that bounds a loop by elapsed time rather than by a count
 * (`while (PENDING(millis(), deadline)) { poll the port; }`) has no bound at all under a frozen
 * clock: nothing inside the loop moves it, so the deadline never arrives. On a board the loop
 * ends on schedule; here it spins for ever, and the test hangs instead of failing.
 *
 * That is not merely inconvenient. Under mutation any mutant that strands such a loop scores as
 * a timeout, timeouts count as detected, and the score is inflated by detections that are of
 * this clock rather than of the defect. It also leaves the firmware's real behaviour there —
 * *leaving the loop when the budget expires* — unreachable, so no test can pin it either.
 *
 * The seam is that such a loop necessarily polls something, and a poll that finds nothing is
 * exactly the case where a real CPU burns cycles. So a test may declare what an empty poll
 * costs, and the loop then terminates for the same reason it does on hardware.
 *
 * Deliberately opt-in and zero by default: a clock that moves when read would surprise every
 * test that does not want it. Two things to know when switching it on:
 *
 *  - It advances time **without firing timer interrupts**, unlike `HAL_test_advance_micros()`.
 *    A test that needs the ISRs to run while it spins needs something else; this exists to end
 *    a wait, not to run the machine during one.
 *  - The cost is per empty poll, so the simulated time a loop consumes depends on how often it
 *    polls. Assert that the loop *ended*, not how long it took.
 */
static uint64_t idle_poll_nanos = 0;

void HAL_test_set_idle_poll_nanos(const uint64_t ns) { idle_poll_nanos = ns; }

void hal_test_idle_poll() { if (idle_poll_nanos) Clock::advance_nanos(idle_poll_nanos); }

// ------------------------
// Serial ports
// ------------------------

MSerialT usb_serial(TERN0(EMERGENCY_PARSER, true));
MSerialT lcd_serial(false);   // a display never sends emergency commands back

/**
 * The display port must not block, and there is nothing to drain it.
 *
 * `HalSerial()` sets `host_connected = true`, and `write()` then busy-waits for room in a
 * 128-byte transmit buffer. On a board the display consumes those bytes; in a test binary
 * nothing does, so the first screen refresh that overflows the buffer spins forever — the
 * suite hangs rather than fails, which is the harness lying about the firmware.
 *
 * Marked as having no host attached, which is the honest model of a test with no display
 * wired up: writes return immediately and the bytes are discarded. Capturing them, so a
 * test could assert what the firmware told the display, needs a drain rather than this flag.
 *
 * In this translation unit and after `lcd_serial` deliberately: initialisation order is
 * only guaranteed within a TU, and this fork has already been bitten once by a static
 * initialiser that ran before the thing it touched (register #20).
 */
namespace {
  struct QuietLcdSerial { QuietLcdSerial() { lcd_serial.host_connected = false; } } _quiet_lcd_serial;
}

// U8glib required functions
extern "C" {
  void u8g_xMicroDelay(uint16_t val) { DELAY_US(val); }
  void u8g_MicroDelay()              { u8g_xMicroDelay(1); }
  void u8g_10MicroDelay()            { u8g_xMicroDelay(10); }
  void u8g_Delay(uint16_t val)       { delay(val); }
}

//************************//

// return free heap space
int freeMemory() { return 0; }

// ------------------------
// ADC
// ------------------------

uint8_t MarlinHAL::active_ch = 0;

uint16_t MarlinHAL::adc_value() {
  const pin_t pin = analogInputToDigitalPin(active_ch);
  if (!isValidPin(pin)) return 0;
  return uint16_t((Gpio::get(pin) >> 2) & 0x3FF); // return 10bit value as Marlin expects
}

void MarlinHAL::reboot() { /* Reset the application state and GPIO */ }

// ------------------------
// BSD String
// ------------------------

/**
 * Copyright (c) 1998, 2015 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef HAS_LIBBSD

  /**
   * Copy string src to buffer dst of size dsize.  At most dsize-1
   * chars will be copied.  Always NUL terminates (unless dsize == 0).
   * Returns strlen(src); if retval >= dsize, truncation occurred.
   */
  size_t MarlinHAL::_strlcpy(char *dst, const char *src, size_t dsize) {
    const char *osrc = src;
    size_t nleft = dsize;

    // Copy as many bytes as will fit.
    if (nleft != 0) while (--nleft != 0) if ((*dst++ = *src++) == '\0') break;

    // Not enough room in dst, add NUL and traverse rest of src.
    if (nleft == 0) {
      if (dsize != 0) *dst = '\0';    // NUL-terminate dst
      while (*src++) { /* nada */ }
    }

    return (src - osrc - 1); // count does not include NUL
  }

#endif // HAS_LIBBSD

#endif // __PLAT_TEST__
