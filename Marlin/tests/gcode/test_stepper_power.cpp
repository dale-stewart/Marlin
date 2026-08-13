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

/**
 * Turning the motors on and off.
 *
 * A stepper motor holds its position by drawing current, so a machine left enabled runs
 * warm forever and one left disabled cannot be trusted to be where it says it is. `M17`
 * and `M18`/`M84` are how a host says which, per axis, and `M18 S<seconds>` sets the
 * timeout after which the firmware decides for itself.
 *
 * Two things are asserted throughout, and the second is the one worth having:
 *
 * - **which** axes changed, and
 * - **which did not**. A command that named X and enabled everything would satisfy every
 *   assertion about X. Almost every test here therefore names one axis and checks a
 *   neighbour was left alone.
 *
 * Note what this file does *not* reach. `do_enable()` and `try_to_disable()` — most of the
 * source — handle boards where two axes share one enable pin, and warn about the axis that
 * came on or stayed on as a side effect. `any_enable_overlap()` is a `constexpr` over the
 * pin assignments and is **false** on this board, so both are dead code here. That is a
 * configuration gap, not a test gap: no test can reach them until a board that shares a
 * pin is among the ones we build.
 */

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/stepper.h"
#include "src/module/planner.h"
#include "src/module/motion.h"
#include "src/MarlinCore.h"
#include "serial_capture.h"
#include <string.h>
#include <stdio.h>
#include "../support/test_clock.h"

namespace {

  /**
   * The enable states and the idle timeout are machine state that outlives a test, and a
   * timeout left set changes what a *later* test's `idle()` does — it starts switching
   * motors off underneath it. Restored both ways round.
   */
  struct StepperPower {
    ena_mask_t was_enabled;
    millis_t was_timeout;
    StepperPower() {
      was_enabled = stepper.axis_enabled.bits;
      was_timeout = TERN(HAS_DISABLE_IDLE_AXES, gcode.stepper_inactive_time, 0);
    }
    ~StepperPower() {
      stepper.axis_enabled.bits = was_enabled;
      TERN_(HAS_DISABLE_IDLE_AXES, gcode.stepper_inactive_time = was_timeout);
    }
  };

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  bool on(const AxisEnum a) { return TEST(stepper.axis_enabled.bits, a); }

  void all_off() { stepper.axis_enabled.bits = 0; }

  // `axis_is_enabled()` takes an extruder index only on multi-extruder builds, so go
  // through the index macro instead and read the same bit it does. Compiles either way.
  #if HAS_EXTRUDERS
    bool e_on(const uint8_t e) { return TEST(stepper.axis_enabled.bits, INDEX_OF_AXIS(E_AXIS, e)); }
  #endif

}

// A bare M17 is "power everything up" — the state a machine wants before being driven.
MARLIN_TEST(stepper_power, M17_with_no_axes_enables_all_of_them) {
  SimulatedMachine machine;
  StepperPower restore;
  all_off();

  host_sends("M17");

  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS), "a bare M17 should enable X");
  TEST_ASSERT_TRUE_MESSAGE(on(Y_AXIS), "and Y");
  TEST_ASSERT_TRUE_MESSAGE(on(Z_AXIS), "and Z");
}

/**
 * Naming an axis enables that one and leaves the rest as they were.
 *
 * The second half is the assertion that matters: `M17 X` on a machine that enabled
 * everything would satisfy any test that only looked at X, and would quietly cost a user
 * the ability to push a Y carriage by hand.
 */
MARLIN_TEST(stepper_power, M17_with_an_axis_enables_only_that_axis) {
  SimulatedMachine machine;
  StepperPower restore;
  all_off();

  host_sends("M17 X");

  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS), "M17 X should enable X");
  TEST_ASSERT_FALSE_MESSAGE(on(Y_AXIS), "and must leave Y alone");
  TEST_ASSERT_FALSE_MESSAGE(on(Z_AXIS), "and Z");
}

// Several axes at once, still not all of them.
MARLIN_TEST(stepper_power, M17_takes_more_than_one_axis_at_a_time) {
  SimulatedMachine machine;
  StepperPower restore;
  all_off();

  host_sends("M17 XZ");

  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS), "M17 XZ should enable X");
  TEST_ASSERT_TRUE_MESSAGE(on(Z_AXIS), "and Z");
  TEST_ASSERT_FALSE_MESSAGE(on(Y_AXIS), "and still leave the axis it did not name alone");
}

MARLIN_TEST(stepper_power, M18_with_an_axis_disables_only_that_axis) {
  SimulatedMachine machine;
  StepperPower restore;

  host_sends("M17");
  host_sends("M18 X");

  TEST_ASSERT_FALSE_MESSAGE(on(X_AXIS), "M18 X should disable X");
  TEST_ASSERT_TRUE_MESSAGE(on(Y_AXIS), "and must leave Y holding its position");
  TEST_ASSERT_TRUE_MESSAGE(on(Z_AXIS), "and Z, which is what stops a gantry dropping");
}

// A bare M18 releases the lot, which is the "I want to move it by hand" case.
MARLIN_TEST(stepper_power, M18_with_no_axes_disables_all_of_them) {
  SimulatedMachine machine;
  StepperPower restore;

  host_sends("M17");
  host_sends("M18");

  TEST_ASSERT_FALSE_MESSAGE(on(X_AXIS), "a bare M18 should disable X");
  TEST_ASSERT_FALSE_MESSAGE(on(Y_AXIS), "and Y");
  TEST_ASSERT_FALSE_MESSAGE(on(Z_AXIS), "and Z");
}

// M84 is the same command under its older name, and hosts in the wild send both.
MARLIN_TEST(stepper_power, M84_is_M18_by_another_name) {
  SimulatedMachine machine;
  StepperPower restore;

  host_sends("M17");
  host_sends("M84 Y");

  TEST_ASSERT_FALSE_MESSAGE(on(Y_AXIS), "M84 Y should disable Y just as M18 Y would");
  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS), "and leave X alone");
}

#if HAS_EXTRUDERS

// `E` with no number means every extruder, which is a different code path from `E<n>`.
MARLIN_TEST(stepper_power, M17_E_enables_the_extruders_without_touching_the_others) {
  SimulatedMachine machine;
  StepperPower restore;
  all_off();

  host_sends("M17 E");

  TEST_ASSERT_TRUE_MESSAGE(e_on(0), "M17 E should enable the extruder");
  TEST_ASSERT_FALSE_MESSAGE(on(X_AXIS), "and not the axes it did not name");
}

MARLIN_TEST(stepper_power, M18_E_disables_the_extruders_without_touching_the_others) {
  SimulatedMachine machine;
  StepperPower restore;

  host_sends("M17");
  host_sends("M18 E");

  TEST_ASSERT_FALSE_MESSAGE(e_on(0), "M18 E should disable the extruder");
  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS), "and leave the motion axes holding position");
}

/**
 * An extruder this machine does not have is not an extruder.
 *
 * `M17 E<n>` indexes an array, so the bound is the difference between enabling a stepper
 * and writing through a pointer past the end of one. The index is derived from `EXTRUDERS`
 * rather than written as a literal, so this says "the first extruder this build does not
 * have" on every configuration rather than only on the one it was written against.
 */
MARLIN_TEST(stepper_power, M17_naming_an_extruder_the_machine_lacks_does_nothing) {
  SimulatedMachine machine;
  StepperPower restore;
  all_off();

  char cmd[16];
  snprintf(cmd, sizeof(cmd), "M17 E%d", EXTRUDERS);
  host_sends(cmd);

  TEST_ASSERT_EQUAL_MESSAGE(0, stepper.axis_enabled.bits,
    "naming an extruder one past the last should enable nothing at all - checked across the "
    "whole mask, because a bound written as <= would set the bit belonging to no stepper "
    "and looking only at extruder zero would not see it");

  // One past the end is the boundary; further out is what separates a bound written as
  // "less than" from one written as "not equal to". The second would let this through and
  // set a bit for a stepper that does not exist.
  snprintf(cmd, sizeof(cmd), "M17 E%d", EXTRUDERS + 1);
  host_sends(cmd);

  TEST_ASSERT_EQUAL_MESSAGE(0, stepper.axis_enabled.bits,
    "and an index well past the end should leave the enable mask untouched, not set a bit "
    "belonging to no stepper");
}

// The other side of the same bound: the last extruder the machine does have works.
MARLIN_TEST(stepper_power, M17_naming_the_first_extruder_enables_it) {
  SimulatedMachine machine;
  StepperPower restore;
  all_off();

  host_sends("M17 E0");

  TEST_ASSERT_TRUE_MESSAGE(e_on(0), "M17 E0 should enable the first extruder");
  TEST_ASSERT_FALSE_MESSAGE(on(X_AXIS), "and nothing else");
}

#if HAS_MULTI_EXTRUDER

/**
 * `E` on its own means *every* extruder, and only a machine with more than one can say so.
 *
 * `M17 E` takes the no-value branch and enables them all; `M17 E0` takes the other and
 * enables one. On a single-extruder build those are the same outcome, so the branch is
 * unasserted there however hard the default configuration is tested — mutating the
 * has-a-value test to "always" survives, because reading a missing value gives 0 and
 * extruder 0 *is* all of them.
 *
 * This is the third time a survivor here has meant "the behaviour needs a machine the
 * measured build is not". The mutation runner only measures `001`; this runs under `003`.
 */
MARLIN_TEST(stepper_power, M17_E_on_its_own_means_every_extruder_not_the_first) {
  SimulatedMachine machine;
  StepperPower restore;
  all_off();

  host_sends("M17 E");

  for (uint8_t e = 0; e < EXTRUDERS; ++e)
    TEST_ASSERT_TRUE_MESSAGE(e_on(e), "M17 E should enable every extruder, not just the first");
}

MARLIN_TEST(stepper_power, M17_E0_means_that_one_extruder_and_not_the_rest) {
  SimulatedMachine machine;
  StepperPower restore;
  all_off();

  host_sends("M17 E0");

  TEST_ASSERT_TRUE_MESSAGE(e_on(0), "M17 E0 should enable the extruder it names");
  TEST_ASSERT_FALSE_MESSAGE(e_on(1), "and leave the others cold");
}

MARLIN_TEST(stepper_power, M18_E_on_its_own_releases_every_extruder) {
  SimulatedMachine machine;
  StepperPower restore;

  host_sends("M17");
  host_sends("M18 E");

  for (uint8_t e = 0; e < EXTRUDERS; ++e)
    TEST_ASSERT_FALSE_MESSAGE(e_on(e), "M18 E should release every extruder");
  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS), "and leave the motion axes alone");
}

#endif // HAS_MULTI_EXTRUDER

#endif // HAS_EXTRUDERS

#if HAS_DISABLE_IDLE_AXES

/**
 * `M18 S<seconds>` is a setting, not a command to switch off.
 *
 * It says "release the motors after this long with nothing to do". Two things have to be
 * true of it and neither is visible in the other: the period is recorded in milliseconds
 * from a value given in seconds, and nothing is disabled *now*. A version that switched off
 * immediately would look identical to a user watching the motors go quiet.
 */
MARLIN_TEST(stepper_power, M18_with_a_time_sets_the_timeout_and_disables_nothing) {
  SimulatedMachine machine;
  StepperPower restore;

  host_sends("M17");
  host_sends("M18 S30");

  TEST_ASSERT_EQUAL_MESSAGE(30000, gcode.stepper_inactive_time,
    "M18 S30 should record thirty seconds as milliseconds");
  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS),
    "and must not switch the motors off now - it is setting a timeout, not pulling the plug");
}

// Zero is the way to say "never", and it has to be distinguishable from a short timeout.
MARLIN_TEST(stepper_power, M18_S0_turns_the_timeout_off_rather_than_setting_a_short_one) {
  SimulatedMachine machine;
  StepperPower restore;

  host_sends("M18 S30");
  host_sends("M18 S0");

  TEST_ASSERT_EQUAL_MESSAGE(0, gcode.stepper_inactive_time,
    "M18 S0 should clear the timeout, not set one of zero length");
}

/**
 * Setting the timeout also restarts the clock on it.
 *
 * `M18 S<n>` calls `reset_stepper_timeout()` before storing the period, so the countdown
 * runs from the command rather than from whenever the machine last moved. Without it a
 * host that sets a timeout on an already-idle machine gets one that has, in effect,
 * already expired — the motors drop on the next idle pass instead of `n` seconds later.
 *
 * Deleting that call survived every assertion on the stored value, because the value is
 * right either way. Time has to pass first for the difference to exist at all.
 */
MARLIN_TEST(stepper_power, setting_the_timeout_restarts_the_countdown) {
  SimulatedMachine machine;
  StepperPower restore;

  host_sends("M17");
  host_sends("M18 S5");
  TestClock::advance_seconds(10);       // longer than the timeout, machine sitting idle

  host_sends("M18 S5");                 // asking again should start the five seconds over
  marlin.idle();

  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS),
    "setting the timeout again should restart it, not leave one that has already run out");
}

/**
 * And the timeout actually fires.
 *
 * This is the behaviour the setting exists for, and asserting the stored number says
 * nothing about it — `manage_inactivity()` is what reads the field, from the idle loop, and
 * it is a different file. Under this HAL `marlin.idle()` costs simulated time, so the wait
 * can be run out in a test the way it runs out on a machine somebody walked away from.
 *
 * Bracketed: checked once before the period is up and once after, because "the motors are
 * off" on its own is satisfied by a timeout of nothing at all.
 */
MARLIN_TEST(stepper_power, the_idle_timeout_releases_the_motors_when_it_expires) {
  SimulatedMachine machine;
  StepperPower restore;
  SerialCapture host;

  host_sends("M17");
  host_sends("M18 S2");
  gcode.reset_stepper_timeout();

  for (uint16_t i = 0; i < 1000; i++) marlin.idle();     // well short of two seconds
  TEST_ASSERT_TRUE_MESSAGE(on(X_AXIS),
    "the motors should still be held before the timeout is up");

  TestClock::advance_seconds(3);
  marlin.idle();
  host.finish();

  TEST_ASSERT_FALSE_MESSAGE(on(X_AXIS),
    "and released once it has passed, which is the whole point of the setting");
}

#endif // HAS_DISABLE_IDLE_AXES

/**
 * Releasing an axis waits for the machine to stop moving first.
 *
 * `M18` with an axis calls `planner.synchronize()` before it cuts the current. Without it
 * the motor is released with a move still queued, so the carriage coasts and the firmware
 * goes on believing it arrived — the position is wrong from then on, silently, and nothing
 * about the enable state says so.
 *
 * A bare statement with no return value, so the only way to see it is a move that takes
 * long enough for the difference to show: with the synchronize the command cannot return
 * until the queue is empty.
 */
MARLIN_TEST(stepper_power, releasing_an_axis_waits_for_the_move_in_progress) {
  SimulatedMachine machine;
  StepperPower restore;

  xyze_pos_t origin = { 0 };
  motion.position = origin;
  planner.set_position_mm(origin);

  host_sends("M17");

  xyze_pos_t target = { 0 }; target.x = 10.0f;
  TEST_ASSERT_TRUE(planner.buffer_line(target, 10.0f));
  TEST_ASSERT_TRUE(planner.has_blocks_queued());

  host_sends("M18 X");

  TEST_ASSERT_FALSE_MESSAGE(planner.has_blocks_queued(),
    "the queued move should have finished before the motor was released");
  TEST_ASSERT_FALSE_MESSAGE(on(X_AXIS), "and then X should be released");
}

