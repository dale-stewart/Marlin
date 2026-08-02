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
 *
 * Time is walked to each interrupt in turn rather than jumped to the end and the
 * interrupts run afterwards. It has to be: a handler reads the clock (Stepper::isr()
 * spins on the timer count to time its step pulses, and programs the next interval from
 * it), so running it at the wrong instant gives it the wrong answers.
 *
 * A handler also *spends* simulated time, which is why nothing here assumes the clock is
 * still where it was left. That includes handlers that advance time themselves: the
 * `advancing` guard means an interrupt cannot recursively run more interrupts, matching
 * hardware, where a handler does not preempt itself.
 */
namespace {
  bool advancing = false;               // interrupts do not nest
  constexpr uint32_t FIRE_BUDGET = 100000;  // bound one call, however far it advances
}

void HAL_test_advance_nanos(const uint64_t ns) {
  const uint64_t target = Clock::nanos() + ns;
  const auto catch_up = [&]() { if (Clock::nanos() < target) Clock::advance_nanos(target - Clock::nanos()); };

  if (advancing) { catch_up(); return; }  // called from inside a handler: time only

  advancing = true;
  for (uint32_t fired = 0; fired < FIRE_BUDGET; fired++) {
    uint8_t soonest = 0xFF;
    uint64_t due = 0, when;
    for (uint8_t i = 0; i < 2; i++)
      if (timers[i].pending(when) && when <= target && (soonest == 0xFF || when < due)) { soonest = i; due = when; }

    if (soonest == 0xFF) break;
    if (due > Clock::nanos()) Clock::advance_nanos(due - Clock::nanos());
    timers[soonest].fire();
  }
  advancing = false;

  catch_up();
}

void HAL_test_advance_millis(const uint32_t ms) { HAL_test_advance_nanos(uint64_t(ms) * 1000000ULL); }
void HAL_test_advance_micros(const uint32_t us) { HAL_test_advance_nanos(uint64_t(us) * 1000ULL); }

#endif // __PLAT_TEST__
