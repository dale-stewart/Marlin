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
 * Endstops and homing.
 *
 * The recorded reason these could not be tested was that `Endstops::update()` only
 * records a hit while the axis is moving towards the switch, and nothing moved. That
 * was the whole of it: with the stepper ISR running on simulated time, a limit switch
 * modelled as a peripheral on the STEP pin (see support/simulated_endstops.h) trips at a
 * chosen physical position and the firmware reacts exactly as it would on a board. No
 * production seam and no HAL change was needed.
 *
 * Note what the endstop pins did *not* need: unlike KILL_PIN and the thermistor inputs,
 * a simulated endstop reads LOW at reset and `X_MIN_ENDSTOP_HIT_STATE` is HIGH, so the
 * firmware sees every switch as open — the state a board powers up in. The hazard that
 * bit the kill button does not exist here.
 *
 * The assertions are about the *relationship* between the switch and the machine rather
 * than about recorded numbers: that the axis stops where the switch is rather than where
 * it was told to go; that a switch behind the direction of travel is ignored; that after
 * homing the coordinate system is re-referenced so the switch reads X_MIN_POS, wherever
 * the switch physically is.
 *
 * Test-HAL only: none of this is reproducible when time is the wall clock.
 */


#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_endstops.h"
#include "src/module/endstops.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/stepper.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include <string.h>

namespace {

  constexpr float SPM = SimulatedMachine::STEPS_PER_MM;

  // A carriage on the X rail with its limit switch at the minimum end.
  struct XRail : SimulatedAxisWithLimit {
    XRail(const float switch_at_mm, const float carriage_at_mm)
      : SimulatedAxisWithLimit(X_STEP_PIN, X_DIR_PIN, ENABLED(INVERT_X_DIR),
                               X_MIN_PIN, X_MIN_ENDSTOP_HIT_STATE,
                               int32_t(switch_at_mm * SPM), int32_t(carriage_at_mm * SPM)) {}
    float mm() const { return float(position()) / SPM; }
    float lowest_mm() const { return float(lowest_reached()) / SPM; }
  };

  // Endstop checking is off until something turns it on; homing turns it on itself.
  struct EndstopsWatching {
    bool was_global;
    EndstopsWatching() {
      was_global = endstops.global_enabled();
      endstops.hit_on_purpose();
      endstops.enable(true);
    }
    ~EndstopsWatching() {
      endstops.enable_globally(was_global);   // restores both the flag and the global
      endstops.hit_on_purpose();
    }
  };

  #if HAS_Y_AXIS
    // The same rail on Y. Homing Y has never been exercised in this configuration, and Y is
    // not X: it has its own pins, its own direction, its own inversion setting and its own
    // entry in every per-axis table homing reads.
    struct YRail : SimulatedAxisWithLimit {
      YRail(const float switch_at_mm, const float carriage_at_mm)
        : SimulatedAxisWithLimit(Y_STEP_PIN, Y_DIR_PIN, ENABLED(INVERT_Y_DIR),
                                 Y_MIN_PIN, Y_MIN_ENDSTOP_HIT_STATE,
                                 int32_t(switch_at_mm * SPM), int32_t(carriage_at_mm * SPM)) {}
      float mm() const { return float(position()) / SPM; }
      float lowest_mm() const { return float(lowest_reached()) / SPM; }
    };
  #endif

  #if HAS_Z_AXIS
    // ...and on Z, which homes against its own minimum switch when there is no probe.
    struct ZRail : SimulatedAxisWithLimit {
      ZRail(const float switch_at_mm, const float carriage_at_mm)
        : SimulatedAxisWithLimit(Z_STEP_PIN, Z_DIR_PIN, ENABLED(INVERT_Z_DIR),
                                 Z_MIN_PIN, Z_MIN_ENDSTOP_HIT_STATE,
                                 int32_t(switch_at_mm * SPM), int32_t(carriage_at_mm * SPM)) {}
      float mm() const { return float(position()) / SPM; }
      float lowest_mm() const { return float(lowest_reached()) / SPM; }
    };
  #endif

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);   // true: skip the "ok" acknowledgement
  }

  // Put the machine, the planner and the steppers all at the same place.
  void machine_is_at_x(const float mm) {
    xyze_pos_t here = { 0 }; here.x = mm;
    motion.position = here;
    planner.set_position_mm(here);
  }

  // The same, for a machine that is about to be homed on more than one axis.
  void machine_is_at(const float x, const float y, const float z) {
    xyze_pos_t here = { 0 };
    NUM_AXIS_CODE(here.x = x, here.y = y, here.z = z, , , , , , );
    motion.position = here;
    planner.set_position_mm(here);
  }

  // Buffer a move straight to an X coordinate, bypassing soft limits.
  bool move_x_to(const float mm, const feedRate_t fr = 20.0f) {
    xyze_pos_t target = motion.position; target.x = mm;
    return planner.buffer_line(target, fr);
  }

}

// ---------------------------------------------------------------------------
// A limit switch, observed
// ---------------------------------------------------------------------------

/**
 * The move ends where the switch is, not where it was told to go.
 *
 * Asserting a step count would not say this: the interesting claim is that the stop
 * point tracks the *switch*, so the target is deliberately well past it.
 */
MARLIN_TEST(endstops, a_moving_axis_stops_where_its_limit_switch_closes) {
  SimulatedMachine machine;
  EndstopsWatching watching;

  XRail x(2.0f, 10.0f);                 // switch at 2 mm, carriage at 10 mm
  machine_is_at_x(10.0f);

  TEST_ASSERT_TRUE(move_x_to(-8.0f));   // 10 mm past the switch
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  TEST_ASSERT_TRUE_MESSAGE(TEST(endstops.trigger_state(), X_MIN), "X_MIN was not recorded");

  // Stopped at the switch, not at the target. Detection is polled, so allow overshoot.
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 2.0f, x.lowest_mm());
}

/**
 * Where it stops is set by the switch alone.
 *
 * Two identical moves against switches 3 mm apart stop 3 mm apart. A test that only
 * checked "it stopped somewhere before the target" would pass on a machine that always
 * stopped after a fixed distance.
 */
MARLIN_TEST(endstops, moving_the_switch_moves_the_stopping_point_with_it) {
  float stopped_at[2];
  const float switch_at[2] = { 2.0f, 5.0f };

  for (uint8_t i = 0; i < 2; i++) {
    SimulatedMachine machine;
    EndstopsWatching watching;
    XRail x(switch_at[i], 12.0f);
    machine_is_at_x(12.0f);
    TEST_ASSERT_TRUE(move_x_to(-8.0f));
    TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
    stopped_at[i] = x.lowest_mm();
  }

  TEST_ASSERT_FLOAT_WITHIN(0.2f, switch_at[1] - switch_at[0], stopped_at[1] - stopped_at[0]);
}

/**
 * A switch behind the carriage is not an obstacle.
 *
 * `Endstops::update()` reads the stepper's direction and only records a hit for the
 * endstop the axis is moving towards, which is what lets a machine drive off a closed
 * switch instead of being stuck against it.
 */
MARLIN_TEST(endstops, a_closed_switch_is_ignored_while_the_axis_moves_away_from_it) {
  SimulatedMachine machine;
  EndstopsWatching watching;

  XRail x(5.0f, 1.0f);                  // carriage starts *inside* the switch
  TEST_ASSERT_TRUE_MESSAGE(x.closed(), "the switch should start closed");
  machine_is_at_x(1.0f);

  TEST_ASSERT_TRUE(move_x_to(20.0f));   // away from the switch
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  TEST_ASSERT_FALSE_MESSAGE(endstops.trigger_state(), "a hit was recorded moving away");
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 20.0f, x.mm());     // the move completed in full
}

/**
 * M121 turns endstop checking off, and then the switch does nothing.
 *
 * This is what a macro relies on when it deliberately drives past a limit.
 */
MARLIN_TEST(endstops, a_disabled_endstop_does_not_stop_the_move) {
  SimulatedMachine machine;
  EndstopsWatching watching;
  endstops.enable(false);

  XRail x(2.0f, 10.0f);
  machine_is_at_x(10.0f);

  TEST_ASSERT_TRUE(move_x_to(0.0f));
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  TEST_ASSERT_TRUE_MESSAGE(x.closures() > 0, "the carriage never reached the switch");
  TEST_ASSERT_FALSE_MESSAGE(endstops.trigger_state(), "a hit was recorded while disabled");
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, x.mm());      // the move completed in full
}

// ---------------------------------------------------------------------------
// Homing
// ---------------------------------------------------------------------------

/**
 * Homing re-references the coordinate system to the switch.
 *
 * That is the whole point of it, and it is what the assertion should say: whatever
 * physical position the switch happens to be at, the machine calls it X_MIN_POS
 * afterwards. Both runs below home to the same *coordinate* from switches 4 mm apart.
 */
MARLIN_TEST(homing, G28_puts_the_origin_wherever_the_switch_is) {
  const float switch_at[2] = { 0.0f, 4.0f };
  float physical[2];

  for (uint8_t i = 0; i < 2; i++) {
    SimulatedMachine machine;
    XRail x(switch_at[i], 30.0f);
    machine_is_at_x(30.0f);
    motion.set_axis_never_homed(X_AXIS);

    host_sends("G28 X");

    TEST_ASSERT_EQUAL_FLOAT(X_MIN_POS, motion.position.x);
    physical[i] = x.mm();
  }

  // Same coordinate, different places: the switches were 4 mm apart and so are they.
  TEST_ASSERT_FLOAT_WITHIN(0.3f, switch_at[1] - switch_at[0], physical[1] - physical[0]);
}

// An axis that needs homing stops needing it, and becomes trusted.
MARLIN_TEST(homing, G28_marks_the_axis_homed) {
  SimulatedMachine machine;
  XRail x(0.0f, 30.0f);
  machine_is_at_x(30.0f);

  motion.set_axis_never_homed(X_AXIS);
  TEST_ASSERT_TRUE(motion.axis_should_home(X_AXIS));
  TEST_ASSERT_FALSE(motion.axis_is_trusted(X_AXIS));

  host_sends("G28 X");

  TEST_ASSERT_FALSE(motion.axis_should_home(X_AXIS));
  TEST_ASSERT_TRUE(motion.axis_is_trusted(X_AXIS));
}

/**
 * Homing drives towards the endstop, not away from it.
 *
 * X_HOME_DIR is -1, so every net movement of the sequence is negative. Asserting the
 * final coordinate alone would not catch a sequence that went the wrong way first.
 */
MARLIN_TEST(homing, G28_moves_towards_the_endstop) {
  SimulatedMachine machine;
  XRail x(0.0f, 30.0f);
  machine_is_at_x(30.0f);
  motion.set_axis_never_homed(X_AXIS);

  host_sends("G28 X");

  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, X_HOME_DIR, "this test assumes X homes to MIN");
  TEST_ASSERT_TRUE_MESSAGE(x.mm() < 30.0f, "the carriage did not move towards the switch");
  // It went to the switch and no further: nothing drove it past by an axis length.
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, x.lowest_mm());
}

/**
 * The switch is touched twice: a fast approach, then a slow re-bump.
 *
 * HOMING_BUMP_MM backs off between the two, so the carriage must retreat by that much
 * and come back. One closure would mean the precision pass never happened.
 */
MARLIN_TEST(homing, G28_bumps_the_switch_twice_with_a_backoff_between) {
  SimulatedMachine machine;
  XRail x(0.0f, 30.0f);
  machine_is_at_x(30.0f);
  motion.set_axis_never_homed(X_AXIS);

  host_sends("G28 X");

  constexpr xyz_float_t bump = HOMING_BUMP_MM;
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, x.closures(), "expected a bump and a re-bump");
  TEST_ASSERT_TRUE_MESSAGE(bump.x > 0, "this test assumes a non-zero HOMING_BUMP_MM");
}

/**
 * Homing finds the switch from the far end of the axis.
 *
 * The seek move is a distance, not a target — the machine does not know where it is, so it
 * drives far enough that the switch must be met whatever the truth was. "Far enough" has to
 * be the whole axis and then some, or a carriage parked at the opposite end stops short and
 * homing reports a failure on a machine that is working perfectly.
 *
 * Starting at the maximum is what makes this a test of the seek distance rather than of
 * homing generally: every other test here starts partway along, where a much shorter seek
 * would do.
 */
MARLIN_TEST(homing, homing_reaches_the_switch_from_the_far_end_of_the_axis) {
  SimulatedMachine machine;
  XRail x(0.0f, X_MAX_POS);
  machine_is_at_x(X_MAX_POS);
  motion.set_axis_never_homed(X_AXIS);

  host_sends("G28 X");

  TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(X_AXIS),
    "homing from the far end of the axis should still find the switch");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(X_MIN_POS, motion.position.x,
    "and should re-reference the origin to it exactly as from anywhere else");
}

/**
 * Homing is idempotent: doing it again lands in the same place.
 *
 * The carriage is driven away from the switch in between, which is what a machine with
 * HOMING_BACKOFF_POST_MM would do for itself. Homing a second time straight off a closed
 * switch is a separate case, covered by
 * `a_move_that_starts_against_a_closed_switch_is_abandoned_at_once`.
 */
MARLIN_TEST(homing, homing_again_lands_in_the_same_place) {
  SimulatedMachine machine;
  XRail x(0.0f, 30.0f);
  machine_is_at_x(30.0f);
  motion.set_axis_never_homed(X_AXIS);

  host_sends("G28 X");
  const float after_first = motion.position.x;
  const float travelled_first = 30.0f - x.mm();

  host_sends("G0 X30 F3000");                 // off the switch and back to the start
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
  motion.set_axis_never_homed(X_AXIS);

  host_sends("G28 X");

  TEST_ASSERT_EQUAL_FLOAT(after_first, motion.position.x);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, travelled_first, 30.0f - x.mm());
}

/**
 * The steppers and the machine coordinate agree once homing is done.
 *
 * `do_homing_move()` zeroes the machine position before each homing move, so the two
 * could disagree without anything else noticing.
 */
MARLIN_TEST(homing, G28_leaves_the_planner_agreeing_with_the_machine) {
  SimulatedMachine machine;
  XRail x(0.0f, 30.0f);
  machine_is_at_x(30.0f);
  motion.set_axis_never_homed(X_AXIS);

  host_sends("G28 X");

  TEST_ASSERT_FALSE(planner.has_blocks_queued());
  TEST_ASSERT_EQUAL_FLOAT(motion.position.x, planner.get_axis_position_mm(X_AXIS));
}

/**
 * Homing X leaves Y and Z alone.
 *
 * `G28 X` selects a single axis; a sequence that homed everything would still satisfy
 * every assertion above.
 */
MARLIN_TEST(homing, G28_X_does_not_home_the_other_axes) {
  SimulatedMachine machine;
  XRail x(0.0f, 30.0f);
  machine_is_at_x(30.0f);

  motion.set_axis_never_homed(X_AXIS);
  motion.set_axis_never_homed(Y_AXIS);

  host_sends("G28 X");

  TEST_ASSERT_FALSE(motion.axis_should_home(X_AXIS));
  TEST_ASSERT_TRUE_MESSAGE(motion.axis_should_home(Y_AXIS), "Y was homed by G28 X");
}

// ---------------------------------------------------------------------------
// Homing the other axes
// ---------------------------------------------------------------------------

/**
 * Homing touches every switch on purpose, and leaves none of them recorded as a hit.
 *
 * An endstop hit is a latched flag, and the rest of the firmware reads it as "something
 * unexpected stopped an axis". Homing closes three switches deliberately, so it has to clear
 * the record on the way out or the next thing to look will believe the machine crashed into
 * something.
 *
 * The clearing is done by `validate_homing_move()`, which `do_homing_move()` calls only for
 * moves heading *towards* a switch — the back-off between the two touches is heading away, and
 * validating that one would report a failure every time. So this pins the flag that tells those
 * two cases apart. Getting it backwards leaves the last touch's hit uncleared, which is what
 * makes the outstanding flag visible here.
 */
MARLIN_TEST(homing, homing_leaves_no_switch_recorded_as_hit) {
  SimulatedMachine machine;
  XRail x(0.0f, 20.0f);
  TERN_(HAS_Y_AXIS, YRail y(0.0f, 20.0f));
  TERN_(HAS_Z_AXIS, ZRail z(0.0f, 8.0f));
  machine_is_at(20.0f, 20.0f, 8.0f);

  motion.set_axis_never_homed(X_AXIS);
  TERN_(HAS_Y_AXIS, motion.set_axis_never_homed(Y_AXIS));
  TERN_(HAS_Z_AXIS, motion.set_axis_never_homed(Z_AXIS));

  host_sends("G28");
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  TEST_ASSERT_FALSE_MESSAGE(motion.axis_should_home(X_AXIS), "X should have been homed");
  TEST_ASSERT_EQUAL_MESSAGE(0, endstops.trigger_state(),
    "homing touches every switch on purpose and should leave none of them recorded as a hit");
}

/**
 * Each axis homes against its own switch.
 *
 * Every per-axis quantity homing reads — the direction, the pin, the inversion, the feedrate,
 * the back-off distance — is a table lookup, and until now only entry zero had ever been read.
 * Putting the three switches at three different places is what makes a lookup that returns the
 * wrong row visible: an axis that homed to X's switch would stop in the wrong place.
 */
MARLIN_TEST(homing, each_axis_homes_against_its_own_switch) {
  SimulatedMachine machine;
  XRail x(1.0f, 25.0f);
  TERN_(HAS_Y_AXIS, YRail y(3.0f, 25.0f));
  TERN_(HAS_Z_AXIS, ZRail z(5.0f, 12.0f));
  machine_is_at(25.0f, 25.0f, 12.0f);

  motion.set_axis_never_homed(X_AXIS);
  TERN_(HAS_Y_AXIS, motion.set_axis_never_homed(Y_AXIS));
  TERN_(HAS_Z_AXIS, motion.set_axis_never_homed(Z_AXIS));

  host_sends("G28");
  TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());

  // Each carriage should have reached down to its own switch, not to another axis's.
  //
  // The low-water mark rather than the final position: what the machine does *after* homing an
  // axis varies by configuration — `Z_SAFE_HOMING` sends the carriage to the middle of the bed
  // before homing Z — and none of that changes which switch each axis went looking for.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 1.0f, x.lowest_mm(), "X should have reached the X switch");
  #if HAS_Y_AXIS
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 3.0f, y.lowest_mm(), "Y should have reached the Y switch");
  #endif
  #if HAS_Z_AXIS
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, 5.0f, z.lowest_mm(), "Z should have reached the Z switch");
  #endif
}

// ---------------------------------------------------------------------------
// A test-HAL defect, pinned rather than fixed
// ---------------------------------------------------------------------------

/**
 * LEGACY-BEHAVIOR: defect register #18 — a test-harness defect, not a firmware one.
 *
 * A move that begins with the switch already closed never finishes — under this HAL.
 *
 * This is not firmware behaviour; on a board the move is discarded on the next step
 * interrupt, which is exactly what the comment above `endstops.update()` in
 * `Stepper::block_phase_isr()` says it arranged. It is `HAL/TEST/hardware/Timer.h`:
 *
 *   void enable() { active = true; schedule(); }
 *
 * `schedule()` restarts the period — `next_fire_ns = now + period`. Real hardware does
 * not do that: `HAL_timer_enable_interrupt()` sets an interrupt-enable bit and leaves
 * the counter running, so a compare match still happens when it was always going to.
 *
 * That difference only matters when something disables and re-enables the step timer
 * more often than the step timer's own period, and one thing does:
 * `Stepper::endstop_triggered()` wraps its body in `ATOMIC_SECTION_START/END`, which is
 * `suspend()` / `wake_up()` — a disable and an enable of MF_TIMER_STEP. While a closed
 * switch sits in front of a moving axis, `Endstops::poll()` calls it from *every*
 * temperature interrupt (~1 ms), and the step interval at the head of a block is longer
 * than that (~2.8 ms here). So the step timer is pushed back before it can ever fire,
 * the abort it was asked to perform is never carried out, `axis_did_move` is never
 * cleared, and the next temperature interrupt asks again. A livelock, with simulated
 * time still advancing.
 *
 * A switch that closes *during* a move does not hit it, because by then the step
 * interval is far shorter than the temperature period — which is why every other test
 * in this file passes.
 *
 * `schedule()` has since been deleted from `enable()`, which is why this test now asserts
 * that the move is abandoned rather than that it stalls. The test is kept because it is
 * the only one that exercises a move *beginning* against a closed switch: every other
 * test in this file closes the switch while the axis is already up to speed, where the
 * step interval is far shorter than the temperature period and the re-arming bug could
 * never have bitten.
 */
MARLIN_TEST(endstops, a_move_that_starts_against_a_closed_switch_is_abandoned_at_once) {
  SimulatedMachine machine;
  EndstopsWatching watching;

  XRail x(5.0f, 1.0f);                  // the carriage is already inside the switch
  TEST_ASSERT_TRUE(x.closed());
  machine_is_at_x(1.0f);

  TEST_ASSERT_TRUE(move_x_to(-5.0f));   // ... and is told to move further into it

  // Half a second of simulated time is ample: the block starts about 100 ms in.
  const bool drained = SimulatedMachine::run_until_idle(500000);
  const bool hit_seen = TEST(endstops.trigger_state(), X_MIN);
  const int32_t moved = x.position() - int32_t(1.0f * SPM);

  // The block is discarded on the next step interrupt, so the queue drains ...
  TEST_ASSERT_TRUE_MESSAGE(drained, "the move never completed");
  TEST_ASSERT_TRUE_MESSAGE(hit_seen, "the hit was not recorded");

  // ... and the carriage does not travel further into the switch to get there.
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, moved, "the carriage moved further into the switch");
}

