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
#ifdef __PLAT_TEST__

#include "../../inc/MarlinConfig.h"
#include "hardware/Timer.h"

// The ISR entry points Marlin defines through HAL_STEP_TIMER_ISR / HAL_TEMP_TIMER_ISR.
extern "C" void TIMER0_IRQHandler();
extern "C" void TIMER1_IRQHandler();

Timer timers[2];

void HAL_timer_init() {
  timers[MF_TIMER_STEP].init(STEPPER_TIMER_RATE, TIMER0_IRQHandler);
  timers[MF_TIMER_TEMP].init(TEMP_TIMER_RATE, TIMER1_IRQHandler);
}

void HAL_timer_start(const uint8_t timer_num, const uint32_t frequency) {
  timers[timer_num].start(frequency);
}

void HAL_timer_enable_interrupt(const uint8_t timer_num)  { timers[timer_num].enable(); }
void HAL_timer_disable_interrupt(const uint8_t timer_num) { timers[timer_num].disable(); }
bool HAL_timer_interrupt_enabled(const uint8_t timer_num) { return timers[timer_num].enabled(); }

void HAL_timer_set_compare(const uint8_t timer_num, const hal_timer_t compare) {
  timers[timer_num].setCompare(compare);
}

hal_timer_t HAL_timer_get_compare(const uint8_t timer_num) {
  return timers[timer_num].getCompare();
}

hal_timer_t HAL_timer_get_count(const uint8_t timer_num) {
  return timers[timer_num].getCount();
}

/**
 * Move simulated time forward, running whatever interrupts fall inside it.
 *
 * This is the whole of the test HAL's scheduler. Time does not pass on its own, so a
 * test that wants the machine to move calls this; a test that does not want it to move
 * simply never does.
 */
void HAL_test_advance_nanos(const uint64_t ns) {
  const uint64_t target = Clock::nanos() + ns;
  Clock::advance_nanos(ns);
  for (uint8_t i = 0; i < 2; i++) timers[i].advance(target);
}

void HAL_test_advance_millis(const uint32_t ms) { HAL_test_advance_nanos(uint64_t(ms) * 1000000ULL); }
void HAL_test_advance_micros(const uint32_t us) { HAL_test_advance_nanos(uint64_t(us) * 1000ULL); }

#endif // __PLAT_TEST__
