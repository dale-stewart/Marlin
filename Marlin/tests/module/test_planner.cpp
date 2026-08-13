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
 * Tests for the motion planner.
 *
 * The planner turns millimetres into steps, enforces the machine's limits, and decides how
 * fast the machine may still be going when it reaches each join between two moves. The
 * arithmetic and the limits are pure and can be asserted directly; the queue behaviour needs
 * a `SimulatedMachine`, both because the planner accepts moves and silently queues nothing
 * while the machine is not running, and because time only advances on request under this HAL.
 *
 * The housekeeping it does on the side — fans, axis activity — is here too, because it is
 * driven from the block being executed rather than from the command that asked for it.
 */

#include "../test/unit_tests.h"
#include "src/module/planner.h"
#include "src/module/temperature.h"
#include "src/module/motion.h"
#include "../support/simulated_machine.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

#include <math.h>
#include <string.h>

namespace {

  struct SavedPlannerSettings {
    planner_settings_t was;
    float was_flow;
    SavedPlannerSettings() { was = planner.settings; was_flow = planner.flow_percentage[0]; }
    ~SavedPlannerSettings() {
      planner.settings = was;
      planner.set_flow(0, was_flow);
      planner.refresh_positioning();
    }
  };

}

MARLIN_TEST(planner, steps_per_mm_converts_millimetres_to_steps) {
  SavedPlannerSettings restore;

  planner.settings.axis_steps_per_mm[X_AXIS] = 80.0f;
  planner.refresh_positioning();
  TEST_ASSERT_EQUAL_FLOAT(1.0f / 80.0f, planner.mm_per_step[X_AXIS]);

  planner.settings.axis_steps_per_mm[X_AXIS] = 160.0f;
  planner.refresh_positioning();
  TEST_ASSERT_EQUAL_FLOAT(1.0f / 160.0f, planner.mm_per_step[X_AXIS]);
}

MARLIN_TEST(planner, flow_scales_the_extrusion_multiplier) {
  SavedPlannerSettings restore;

  planner.set_flow(0, 100);
  const float at_100 = planner.e_factor[0];

  planner.set_flow(0, 200);
  TEST_ASSERT_EQUAL_FLOAT(at_100 * 2.0f, planner.e_factor[0]);

  planner.set_flow(0, 50);
  TEST_ASSERT_EQUAL_FLOAT(at_100 / 2.0f, planner.e_factor[0]);
}

MARLIN_TEST(planner, a_flow_of_zero_extrudes_nothing) {
  SavedPlannerSettings restore;
  planner.set_flow(0, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, planner.e_factor[0]);
}

// The queue starts empty and reports itself so.
MARLIN_TEST(planner, an_empty_queue_has_nothing_planned) {
  planner.clear_block_buffer();
  TEST_ASSERT_EQUAL(0, planner.movesplanned());
  TEST_ASSERT_FALSE(planner.has_blocks_queued());
}

MARLIN_TEST(planner, feedrate_and_acceleration_limits_are_kept_per_axis) {
  SavedPlannerSettings restore;

  planner.settings.max_feedrate_mm_s[X_AXIS] = 300.0f;
  planner.settings.max_feedrate_mm_s[Y_AXIS] = 200.0f;
  TEST_ASSERT_EQUAL_FLOAT(300.0f, planner.settings.max_feedrate_mm_s[X_AXIS]);
  TEST_ASSERT_EQUAL_FLOAT(200.0f, planner.settings.max_feedrate_mm_s[Y_AXIS]);

  planner.settings.max_acceleration_mm_per_s2[X_AXIS] = 1000;
  planner.refresh_acceleration_rates();
  TEST_ASSERT_EQUAL(1000, planner.settings.max_acceleration_mm_per_s2[X_AXIS]);
}

// Setting a position tells the planner where the machine is without moving it.
MARLIN_TEST(planner, set_position_records_where_the_machine_is) {
  SavedPlannerSettings restore;
  planner.settings.axis_steps_per_mm[X_AXIS] = 80.0f;
  planner.refresh_positioning();

  xyze_pos_t here = { 0 };
  here.x = 12.5f;
  planner.set_position_mm(here);
  TEST_ASSERT_EQUAL_FLOAT(12.5f, planner.get_axis_position_mm(X_AXIS));
}

#if HAS_VOLUMETRIC_EXTRUSION

namespace {

  void host_sends(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  // Volumetric mode and the filament size outlive the command that set them, so both are put
  // back — a later test asking for millimetres of filament would otherwise get millimetres
  // cubed and be wrong by a factor of the cross-section.
  struct SavedVolumetric {
    bool was_enabled;
    float was_size;
    SavedVolumetric() : was_enabled(parser.volumetric_enabled), was_size(planner.filament_size[0]) {}
    ~SavedVolumetric() {
      planner.filament_size[0] = was_size;
      parser.volumetric_enabled = was_enabled;
      planner.calculate_volumetric_multipliers();
    }
  };

  // The area of a circle of this diameter, computed here rather than taken from the firmware.
  float cross_section(const float diameter) {
    const float r = diameter * 0.5f;
    return float(M_PI) * r * r;
  }

}

/**
 * In volumetric mode an E value is a volume, and the filament fed is that volume over its
 * cross-section.
 *
 * That is the whole of `M200`: the slicer stops caring what diameter the filament is and
 * asks for cubic millimetres of plastic, and the firmware divides by the area to get a
 * length. So the multiplier must be the reciprocal of the area — asserted against a circle
 * area computed in the test, not against the firmware's own macro.
 *
 * Two diameters, because a single one would be satisfied by any constant that happened to
 * match, and because the relation is quadratic: doubling the diameter must quarter the
 * multiplier, which no linear mistake can produce.
 */
MARLIN_TEST(planner, volumetric_mode_divides_by_the_filament_cross_section) {
  SavedVolumetric restore;

  host_sends("M200 D1.75");
  TEST_ASSERT_TRUE_MESSAGE(parser.volumetric_enabled, "a diameter should switch volumetric on");
  const float thin = planner.volumetric_multiplier[0];
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 1.0f / cross_section(1.75f), thin,
    "the multiplier should be the reciprocal of the filament's cross-section");

  host_sends("M200 D3.5");
  const float thick = planner.volumetric_multiplier[0];
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 1.0f / cross_section(3.5f), thick,
    "and again at another diameter");

  // Twice the diameter is four times the area, so a quarter of the multiplier.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 4.0f, thin / thick,
    "doubling the diameter should quarter the multiplier");
}

/**
 * `M200 D0` and `M200 S0` both mean millimetres again.
 *
 * A multiplier of exactly one is what "not volumetric" means, and there are two ways to ask
 * for it: no diameter, or the mode switched off with a diameter still remembered. The second
 * is the one worth pinning — the size stays set, so a machine that only looked at the
 * diameter would keep dividing by an area nobody asked it to use.
 */
MARLIN_TEST(planner, switching_volumetric_off_returns_E_to_millimetres) {
  SavedVolumetric restore;

  host_sends("M200 D1.75");
  TEST_ASSERT_TRUE_MESSAGE(planner.volumetric_multiplier[0] < 0.9f,
    "the multiplier should be well away from one while volumetric is on");

  host_sends("M200 S0");
  TEST_ASSERT_FALSE_MESSAGE(parser.volumetric_enabled, "S0 should switch volumetric off");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 1.0f, planner.volumetric_multiplier[0],
    "with volumetric off an E value is a length again, whatever diameter is remembered");

  host_sends("M200 D0");
  TEST_ASSERT_FALSE_MESSAGE(parser.volumetric_enabled, "D0 should also switch volumetric off");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, 1.0f, planner.volumetric_multiplier[0],
    "and should leave the multiplier at one");
}

/**
 * Flow rate and volumetric scaling compose.
 *
 * They are separate corrections applied to the same number — one for the filament's
 * geometry, one for the operator's judgement — and the planner keeps their product. Asserting
 * the product rather than either alone is what says neither has replaced the other, which is
 * the way this goes wrong.
 */
MARLIN_TEST(planner, flow_rate_and_volumetric_scaling_multiply) {
  SavedVolumetric restore;
  SavedPlannerSettings restore_settings;

  host_sends("M200 D1.75");
  host_sends("M221 S75");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.75f / cross_section(1.75f), planner.e_factor[0],
    "the applied factor should be the flow rate times the volumetric multiplier");

  // Either one alone leaves the other in place.
  host_sends("M221 S100");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 1.0f / cross_section(1.75f), planner.e_factor[0],
    "restoring the flow rate should leave the volumetric multiplier applied");
}

#endif // HAS_VOLUMETRIC_EXTRUSION

//
// ---- Holding back the first block ----
//

namespace {

  // Start from an empty queue with the delivery delay armed, which is the state the planner
  // is in whenever the machine has just run out of work.
  //
  // A `SimulatedMachine` is needed even though nothing here moves: the planner refuses to
  // queue anything while the machine is not running, and says so by accepting the move and
  // quietly producing no block.
  void queue_moves(const size_t moves) {
    planner.clear_block_buffer();
    xyze_pos_t at = { 0 };
    planner.set_position_mm(at);
    for (size_t i = 1; i <= moves; i++) {
      at.x = float(i);
      TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(at, 10.0f), "the move was not accepted");
    }
  }

  // How many times the stepper would have asked for work and been told to wait, up to a cap.
  size_t refusals_before_a_block(const size_t give_up_after = 1000) {
    for (size_t i = 0; i < give_up_after; i++)
      if (planner.get_current_block()) return i;
    return give_up_after;
  }

}

/**
 * A single queued move is not started straight away.
 *
 * A move delivered the instant it arrives has to be planned as if it were the last one — it
 * must decelerate to a stop at its own end, because nothing is known about what follows. Hold
 * it for a moment and the moves after it can be planned with it, and the machine runs through
 * the join instead of stopping at it.
 *
 * So the queue going empty is not a reason to start immediately; it is a reason to wait.
 */
MARLIN_TEST(planner, a_lone_move_is_not_delivered_immediately) {
  SimulatedMachine machine;
  queue_moves(1);

  TEST_ASSERT_NULL_MESSAGE(planner.get_current_block(),
    "the first move after an empty queue should be held back, not started at once");

  planner.clear_block_buffer();
}

/**
 * Enough queued work is delivered at once.
 *
 * The hold exists to collect moves to plan together, so it has no purpose once there are
 * some. A machine that waited anyway would stall for the delay at the start of every job,
 * and again after every pause.
 */
MARLIN_TEST(planner, a_queue_with_enough_work_is_delivered_at_once) {
  SimulatedMachine machine;
  queue_moves(3);

  TEST_ASSERT_NOT_NULL_MESSAGE(planner.get_current_block(),
    "a queue with several moves in it has nothing to wait for");

  planner.clear_block_buffer();
}

/**
 * The hold ends when the work arrives, not when the clock runs out.
 *
 * There are two ways out of the wait and this is the one that matters: the counter is a
 * fallback for a host that sends one move and stops, while the ordinary case is that more
 * moves turn up and the machine should get on with it. Testing only the counter would leave
 * "wait the full delay every time" indistinguishable from correct.
 */
MARLIN_TEST(planner, more_moves_arriving_release_the_held_block) {
  SimulatedMachine machine;
  queue_moves(1);

  // Still waiting, and nowhere near the end of the fallback counter.
  for (uint8_t i = 0; i < 5; i++)
    TEST_ASSERT_NULL_MESSAGE(planner.get_current_block(), "the lone move should still be held");

  // Two more arrive. Now there is something to plan with.
  xyze_pos_t at = { 0 };
  at.x = 2.0f; TEST_ASSERT_TRUE(planner.buffer_line(at, 10.0f));
  at.x = 3.0f; TEST_ASSERT_TRUE(planner.buffer_line(at, 10.0f));

  TEST_ASSERT_NOT_NULL_MESSAGE(planner.get_current_block(),
    "the block should be released as soon as there is enough queued to plan with");

  planner.clear_block_buffer();
}

/**
 * The wait is a delay, not a deadlock.
 *
 * The fallback: a host that sends a single move and then goes quiet must still see it run.
 * Asserting that it eventually arrives is what says the counter runs out rather than the
 * machine waiting for a third move that is never coming.
 */
MARLIN_TEST(planner, a_lone_move_is_delivered_once_the_wait_expires) {
  SimulatedMachine machine;
  queue_moves(1);

  const size_t refusals = refusals_before_a_block();
  TEST_ASSERT_TRUE_MESSAGE(refusals > 0, "the lone move should have been held at least once");
  TEST_ASSERT_TRUE_MESSAGE(refusals < 1000,
    "a lone move must eventually run, or a host that sends one move would hang the machine");

  planner.clear_block_buffer();
}

#if HAS_JUNCTION_DEVIATION && HAS_EXTRUDERS

namespace {

  // Long enough that the machine can reach the junction speed within one leg, so what is
  // read back is the corner's own limit and not "as fast as it could get to here".
  constexpr float LEG_MM = 10.0f;

  struct Join {
    float entry_speed_sqr;  // the limit the planner put on the speed through the join
    float acceleration;     // the second block's own acceleration, in its own units
  };

  // Plan two moves meeting at `via`, and report what the planner decided about the join.
  Join plan_corner(const xyze_pos_t &from, const xyze_pos_t &via, const xyze_pos_t &to,
                   const float feedrate = 60.0f) {
    planner.clear_block_buffer();
    planner.set_position_mm(from);
    TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(via, feedrate), "the first leg was not accepted");
    TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(to, feedrate), "the second leg was not accepted");
    TEST_ASSERT_EQUAL_MESSAGE(2, planner.movesplanned(), "both legs should still be queued");

    const uint8_t last = (planner.block_buffer_head + BLOCK_BUFFER_SIZE - 1) % (BLOCK_BUFFER_SIZE);
    const block_t &second = planner.block_buffer[last];
    return { second.max_entry_speed_sqr, second.acceleration };
  }

  // Junction deviation: a corner of half-angle θ/2 is taken as if it were an arc that strays
  // `junction_deviation_mm` from the true corner. Given the cosine of the angle between the
  // two paths, this is the speed that allows.
  float speed_sqr_for_cosine(const float accel, const float cos_theta) {
    const float sin_half = sqrtf(0.5f * (1.0f - cos_theta));
    return accel * planner.junction_deviation_mm * sin_half / (1.0f - sin_half);
  }

  // The cosine the planner should compute for a right-angle turn in XY with `e` millimetres
  // of filament pushed along each leg. The extruder does not turn the corner — it runs
  // straight through it — so in the four-dimensional space the planner works in, the two
  // paths are not perpendicular at all: their dot product is the E term alone, and each
  // leg's length is √(leg² + e²).
  float cosine_of_a_right_angle_while_extruding(const float e) {
    return -(e * e) / (LEG_MM * LEG_MM + e * e);
  }

  /**
   * Extrusion that reaches the planner, at one millimetre out per millimetre in.
   *
   * Two settings, and the second is the one that used to be assumed. `e_factor` is the
   * flow multiplier. `allow_cold_extrude` decides whether the E part of a move survives
   * at all: `Planner::buffer_line()` calls `tooColdToExtrude()` and, if the nozzle is
   * below EXTRUDE_MINTEMP, sets `position.e = target.e` and zeroes the E steps — silently,
   * with no complaint on any channel and `buffer_line()` still returning true. Every
   * assertion about an extruding corner then reads as a travel corner.
   *
   * These tests used to inherit that allowance from whichever earlier test had last set
   * it, which is not a precondition at all. `014-pid_bed` is what exposed it: three new
   * tests earlier in the run changed what was left behind, and two corner tests began
   * reporting the travel-corner value in a configuration that has nothing to do with
   * corners. A fixture has to state every setting the behaviour depends on.
   */
  struct PlainExtrusion {
    float was_e_factor;
    bool was_cold_ok;
    PlainExtrusion() {
      was_e_factor = planner.e_factor[0]; planner.e_factor[0] = 1.0f;
      was_cold_ok = thermalManager.allow_cold_extrude; thermalManager.allow_cold_extrude = true;
    }
    ~PlainExtrusion() {
      planner.e_factor[0] = was_e_factor;
      thermalManager.allow_cold_extrude = was_cold_ok;
      planner.clear_block_buffer();
    }
  };

}

/**
 * A right-angle turn between two travel moves is a right-angle turn.
 *
 * The baseline for the two tests below, and the other arm of the branch they are about.
 * With nothing being extruded the corner is what it looks like: the paths meet at 90°,
 * their unit vectors are perpendicular, and the cosine between them is zero.
 */
MARLIN_TEST(planner, a_travel_corner_is_as_sharp_as_it_looks) {
  SimulatedMachine machine;
  PlainExtrusion plain;

  xyze_pos_t from = { 0 }, via = { 0 }, to = { 0 };
  via.x = LEG_MM;
  to.x = LEG_MM; to.y = LEG_MM;

  const Join join = plan_corner(from, via, to);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f * speed_sqr_for_cosine(join.acceleration, 0.0f),
    speed_sqr_for_cosine(join.acceleration, 0.0f), join.entry_speed_sqr,
    "a 90 degree travel corner should be planned as a 90 degree corner");
}

/**
 * Extruding through a corner makes it a gentler corner.
 *
 * The extruder is a fourth direction, and it does not reverse at the join — filament keeps
 * going the same way through it. So the angle the planner has to slow for is the angle
 * between the two moves *including* E, which is always wider than the angle drawn on the
 * bed. Pushing one millimetre of filament per millimetre of travel turns a right angle into
 * a 120 degree bend, and the machine may take it faster.
 *
 * This only works if the junction vector is normalised across all four axes. Scaling it by
 * the pre-calculated 1/XYZ-length instead leaves E oversized, and every extruding corner
 * then looks like a straight line.
 */
MARLIN_TEST(planner, extruding_through_a_corner_widens_it) {
  SimulatedMachine machine;
  PlainExtrusion plain;

  xyze_pos_t from = { 0 }, via = { 0 }, to = { 0 };
  via.x = LEG_MM;            via.e = LEG_MM;
  to.x = LEG_MM; to.y = LEG_MM; to.e = 2.0f * LEG_MM;

  const Join join = plan_corner(from, via, to);

  const float cos_theta = cosine_of_a_right_angle_while_extruding(LEG_MM);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, -0.5f, cos_theta,
    "one millimetre of filament per millimetre of travel should give a 120 degree bend");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f * speed_sqr_for_cosine(join.acceleration, cos_theta),
    speed_sqr_for_cosine(join.acceleration, cos_theta), join.entry_speed_sqr,
    "the corner should be planned as the wider one the extruder makes it");
}

/**
 * ...and how much gentler depends on how much filament.
 *
 * Half the extrusion is half the E component, so the same right angle is a narrower bend and
 * a slower one. Two points on the curve rather than one: a single extruding corner would be
 * satisfied by any rule that widens corners at all, including one that ignores the amount.
 */
MARLIN_TEST(planner, less_filament_through_the_same_corner_widens_it_less) {
  SimulatedMachine machine;
  PlainExtrusion plain;

  const float e = LEG_MM / 2.0f;
  xyze_pos_t from = { 0 }, via = { 0 }, to = { 0 };
  via.x = LEG_MM;            via.e = e;
  to.x = LEG_MM; to.y = LEG_MM; to.e = 2.0f * e;

  const Join join = plan_corner(from, via, to);

  const float cos_theta = cosine_of_a_right_angle_while_extruding(e);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f * speed_sqr_for_cosine(join.acceleration, cos_theta),
    speed_sqr_for_cosine(join.acceleration, cos_theta), join.entry_speed_sqr,
    "half the filament should widen the corner by half as much");

  // ...and slower than the fully-extruding corner, which is the direction the amount acts in.
  TEST_ASSERT_TRUE_MESSAGE(
    speed_sqr_for_cosine(join.acceleration, cos_theta)
      < speed_sqr_for_cosine(join.acceleration, cosine_of_a_right_angle_while_extruding(LEG_MM)),
    "less filament should mean a narrower bend and a lower speed through it");
}

#endif // HAS_JUNCTION_DEVIATION && HAS_EXTRUDERS

#if HAS_Z_AXIS

namespace {

  // Plan one move on an empty queue and hand back the block it produced. One move rather than
  // several, because a queue with two or more in it is also being slowed by the buffer-drain
  // rule, and that is a different limit.
  const block_t& plan_one(const xyze_pos_t &to, const float feedrate) {
    planner.clear_block_buffer();
    xyze_pos_t origin = { 0 };
    planner.set_position_mm(origin);
    TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(to, feedrate), "the move was not accepted");
    TEST_ASSERT_EQUAL_MESSAGE(1, planner.movesplanned(), "exactly one move should be queued");
    const uint8_t last = (planner.block_buffer_head + BLOCK_BUFFER_SIZE - 1) % (BLOCK_BUFFER_SIZE);
    return planner.block_buffer[last];
  }

  // A fast pair of horizontal axes and a slow vertical one — the shape every cartesian machine
  // has, and the reason the limit is per axis rather than on the feedrate. Stated here rather
  // than inherited, because `SimulatedMachine` flattens all of them to 300 and the whole point
  // of these tests is that they differ.
  constexpr float FAST_MM_S = 300.0f, SLOW_MM_S = 5.0f;

  struct MixedAxisSpeeds {
    SimulatedMachine machine;
    MixedAxisSpeeds() {
      planner.settings.max_feedrate_mm_s[X_AXIS] = FAST_MM_S;
      TERN_(HAS_Y_AXIS, planner.settings.max_feedrate_mm_s[Y_AXIS] = FAST_MM_S);
      planner.settings.max_feedrate_mm_s[Z_AXIS] = SLOW_MM_S;
    }
    ~MixedAxisSpeeds() { planner.clear_block_buffer(); }
  };

  // The speed along the path at which `axis` is running at exactly `limit`. A move is a
  // direction and a speed; each axis gets the fraction of that speed its share of the
  // displacement calls for, so the path speed that puts one axis on its limit is that limit
  // divided by the axis's share.
  float path_speed_putting_axis_at(const float limit, const float axis_mm, const float path_mm) {
    return limit * path_mm / axis_mm;
  }

}

/**
 * A move no faster than its axes allow is given the feedrate it asked for.
 *
 * The baseline, and the other arm of the correction below. Nothing about a diagonal in the
 * horizontal plane troubles a machine whose horizontal axes are fast, so nothing should
 * happen to it.
 */
MARLIN_TEST(planner, a_move_within_every_axis_limit_keeps_its_feedrate) {
  MixedAxisSpeeds limits;

  xyze_pos_t to = { 0 }; to.x = 10.0f; TERN_(HAS_Y_AXIS, to.y = 10.0f);
  const block_t &block = plan_one(to, 100.0f);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 100.0f, block.nominal_speed,
    "a move inside every axis limit should run at the speed it was given");
}

/**
 * One axis over its limit holds the whole move back.
 *
 * The machine cannot slow the offending axis alone: the others are tied to it by the shape of
 * the move, and easing off one of four axes would bend the path. So the entire move is scaled
 * until the worst axis is exactly at its limit, and the path is walked more slowly rather than
 * differently.
 *
 * The assertion is that the slow axis ends up *exactly* on its limit — not merely that
 * something was slowed down. A rule that halved the feedrate whenever any axis complained
 * would also produce a slower move.
 */
MARLIN_TEST(planner, an_axis_over_its_limit_slows_the_whole_move) {
  MixedAxisSpeeds limits;

  xyze_pos_t to = { 0 }; to.x = 20.0f; to.z = 10.0f;
  const float path_mm = sqrtf(20.0f * 20.0f + 10.0f * 10.0f);

  // Asked for 100 mm/s along the path, Z alone would be doing 44.7 mm/s — nine times its limit.
  const block_t &block = plan_one(to, 100.0f);

  const float expected = path_speed_putting_axis_at(SLOW_MM_S, 10.0f, path_mm);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, expected, block.nominal_speed,
    "the move should be slowed until Z is exactly at its own limit");

  // ...and X came down with it, in the same proportion, so the move still goes where it was
  // pointed. Twice the displacement of Z, so twice the speed.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 2.0f * SLOW_MM_S, block.nominal_speed * 20.0f / path_mm,
    "X should have been scaled by the same factor, leaving the direction of the move alone");
}

/**
 * With two axes over their limits, the tighter one decides.
 *
 * A rule that took the first axis it found over the limit, or the last, would satisfy the test
 * above. Here both X and Z are asked for more than they can do, and the two answers are three
 * times apart: the move must come down to the lower of them, which leaves X comfortably inside
 * its own limit rather than on it.
 */
MARLIN_TEST(planner, the_tightest_axis_limit_is_the_one_that_binds) {
  MixedAxisSpeeds limits;

  xyze_pos_t to = { 0 }; to.x = 200.0f; to.z = 10.0f;
  const float path_mm = sqrtf(200.0f * 200.0f + 10.0f * 10.0f);

  const float if_only_x_bound = path_speed_putting_axis_at(FAST_MM_S, 200.0f, path_mm),
              if_only_z_bound = path_speed_putting_axis_at(SLOW_MM_S, 10.0f, path_mm);
  TEST_ASSERT_TRUE_MESSAGE(if_only_z_bound < if_only_x_bound,
    "this test needs Z to be the tighter of the two limits");

  const block_t &block = plan_one(to, 400.0f);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-2f, if_only_z_bound, block.nominal_speed,
    "the lower of the two limits should be the one that binds");
  TEST_ASSERT_TRUE_MESSAGE(block.nominal_speed * 200.0f / path_mm < FAST_MM_S,
    "X should be left inside its limit rather than on it");
}

/**
 * ...and which axis that is depends on the move, not on the axis.
 *
 * The same two limits, and now the shallow climb makes X the binding one: a hundred
 * millimetres across for one up asks far more of X than of Z. The pair matters — a test where
 * the slow axis always wins would pass against an implementation that only ever looked at Z.
 */
MARLIN_TEST(planner, a_fast_axis_binds_when_the_move_asks_more_of_it) {
  MixedAxisSpeeds limits;

  xyze_pos_t to = { 0 }; to.x = 100.0f; to.z = 1.0f;
  const float path_mm = sqrtf(100.0f * 100.0f + 1.0f);

  const float if_only_x_bound = path_speed_putting_axis_at(FAST_MM_S, 100.0f, path_mm),
              if_only_z_bound = path_speed_putting_axis_at(SLOW_MM_S, 1.0f, path_mm);
  TEST_ASSERT_TRUE_MESSAGE(if_only_x_bound < if_only_z_bound,
    "this test needs X to be the tighter of the two limits");

  const block_t &block = plan_one(to, 400.0f);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-2f, if_only_x_bound, block.nominal_speed,
    "X should be the axis that binds here");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 3.0f, block.nominal_speed * 1.0f / path_mm,
    "Z should be left well inside its limit, at three fifths of it");
}

#endif // HAS_Z_AXIS

/**
 * The tightest limit wins wherever in the order it happens to sit.
 *
 * The two tests above both have the binding axis last in the order the axes are checked, so a
 * planner that simply took the last complaint would pass them. This one puts the tighter limit
 * first: X and Z are both asked for more than they can do, and X — checked first — is the one
 * that must decide. Half again as far apart as the answers, so nothing in the tolerance can
 * confuse them.
 */
MARLIN_TEST(planner, the_order_the_axes_are_checked_in_does_not_decide) {
  MixedAxisSpeeds limits;

  constexpr float TIGHT_MM_S = 10.0f, LOOSE_MM_S = 20.0f;
  planner.settings.max_feedrate_mm_s[X_AXIS] = TIGHT_MM_S;
  planner.settings.max_feedrate_mm_s[Z_AXIS] = LOOSE_MM_S;

  xyze_pos_t to = { 0 }; to.x = 10.0f; to.z = 10.0f;
  const float path_mm = sqrtf(200.0f);

  const float if_only_x_bound = path_speed_putting_axis_at(TIGHT_MM_S, 10.0f, path_mm),
              if_only_z_bound = path_speed_putting_axis_at(LOOSE_MM_S, 10.0f, path_mm);
  TEST_ASSERT_TRUE_MESSAGE(if_only_x_bound < if_only_z_bound,
    "this test needs the first axis checked to be the tighter one");

  const block_t &block = plan_one(to, 100.0f);

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, if_only_x_bound, block.nominal_speed,
    "the tightest limit should bind even when a looser one is found after it");
}

namespace {

  // Two moves in a straight line, the first `run_up_mm` long and the second long enough that
  // nothing about stopping at the end of it reaches back. Reports the second block.
  const block_t& plan_a_run_up(const float run_up_mm, const float feedrate = 60.0f) {
    planner.clear_block_buffer();
    xyze_pos_t at = { 0 };
    planner.set_position_mm(at);
    at.x = run_up_mm;
    TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(at, feedrate), "the run-up was not accepted");
    at.x = run_up_mm + 50.0f;
    TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(at, feedrate), "the second move was not accepted");
    TEST_ASSERT_EQUAL_MESSAGE(2, planner.movesplanned(), "both moves should still be queued");
    const uint8_t last = (planner.block_buffer_head + BLOCK_BUFFER_SIZE - 1) % (BLOCK_BUFFER_SIZE);
    return planner.block_buffer[last];
  }

  const block_t& the_block_before(const block_t &b) {
    const uint8_t here = uint8_t(&b - planner.block_buffer);
    return planner.block_buffer[(here + BLOCK_BUFFER_SIZE - 1) % (BLOCK_BUFFER_SIZE)];
  }

  // v² = u² + 2as, over the whole of a block.
  float speed_sqr_reachable_across(const block_t &b) {
    return b.entry_speed_sqr + 2.0f * b.acceleration * b.millimeters;
  }

}

/**
 * A move cannot start faster than the one before it managed to reach.
 *
 * The corner between two collinear moves permits any speed at all, and the second move here is
 * long enough to stop from comfortably. What holds it back is behind it: the first move is half
 * a millimetre of runway, and from a standstill that is not enough room to reach the commanded
 * feedrate. The planner's forward pass is what notices — it walks the queue from the front
 * carrying "the fastest we can be by now" and pulls each entry speed down to it.
 *
 * Asserted as v² = u² + 2as across the first block, which is the whole of the claim.
 */
MARLIN_TEST(planner, a_move_cannot_enter_faster_than_the_run_up_allows) {
  SimulatedMachine machine;

  const block_t &second = plan_a_run_up(0.5f);
  const block_t &first = the_block_before(second);

  // Stated against the commanded feedrate and not against `max_entry_speed_sqr`: the forward
  // pass writes its answer back into that field as well, to mark the block as already dealt
  // with, so by the time a test can read it it agrees with whatever happened.
  TEST_ASSERT_TRUE_MESSAGE(speed_sqr_reachable_across(first) < sq(second.nominal_speed),
    "this test needs a run-up too short to reach the speed that was asked for");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f * speed_sqr_reachable_across(first),
    speed_sqr_reachable_across(first), second.entry_speed_sqr,
    "the second move should enter at exactly the speed the first one could reach");

  planner.clear_block_buffer();
}

/**
 * Given room to get up to speed, the move before decides nothing.
 *
 * The other arm. Fifty millimetres of runway is more than enough at this feedrate, so the
 * forward pass finds nothing to correct and the entry speed is whatever the corner and the
 * commanded feedrate already allowed. Without this, "always enter as slowly as the run-up
 * permits" and "enter as fast as everything permits" are the same rule.
 */
MARLIN_TEST(planner, a_long_enough_run_up_holds_nothing_back) {
  SimulatedMachine machine;

  const block_t &second = plan_a_run_up(50.0f);
  const block_t &first = the_block_before(second);

  TEST_ASSERT_TRUE_MESSAGE(speed_sqr_reachable_across(first) > sq(second.nominal_speed),
    "this test needs a run-up long enough to reach the speed that was asked for");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f * second.max_entry_speed_sqr,
    second.max_entry_speed_sqr, second.entry_speed_sqr,
    "with room to get up to speed the second move should enter as fast as it is allowed to");

  planner.clear_block_buffer();
}

/**
 * Twice the runway is twice the square of the speed.
 *
 * The relation rather than a pair of numbers: v² = 2as is linear in the distance, so doubling
 * the run-up doubles the square of the speed reached — a factor of √2 on the speed itself.
 * A rule that clamped a short first move to some fixed fraction of the feedrate would satisfy
 * both tests above and neither of these two points together.
 */
MARLIN_TEST(planner, the_speed_a_run_up_reaches_grows_with_its_length) {
  SimulatedMachine machine;

  const float short_run = plan_a_run_up(0.25f).entry_speed_sqr;
  const float long_run = plan_a_run_up(0.5f).entry_speed_sqr;

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f * short_run, short_run, long_run / 2.0f,
    "twice the run-up should give twice the square of the entry speed");

  planner.clear_block_buffer();
}

#if HAS_FAN && PIN_EXISTS(PART_COOLING_FAN0)

namespace {

  // Every fan's output pin, in order. Named individually because the pin macros are
  // per-fan tokens rather than an array the firmware could be asked to index.
  const pin_t FAN_PINS[] = {
    PART_COOLING_FAN0_PIN, PART_COOLING_FAN1_PIN, PART_COOLING_FAN2_PIN, PART_COOLING_FAN3_PIN,
    PART_COOLING_FAN4_PIN, PART_COOLING_FAN5_PIN, PART_COOLING_FAN6_PIN, PART_COOLING_FAN7_PIN
  };

  // What a fan is actually being driven at, as opposed to what was asked for.
  uint16_t fan_output(const uint8_t f = 0) { return Gpio::get(FAN_PINS[f]); }

  // `check_axes_activity()` is the housekeeping pass the main loop runs; the fan follows from
  // it rather than from the command that set the speed.
  void the_housekeeping_pass_runs() { planner.check_axes_activity(); }

}

/**
 * With nothing to print, the fan follows the command.
 *
 * Two speeds rather than one, because a single speed is satisfied by a machine that drives the
 * fan at that value whatever it was told.
 */
MARLIN_TEST(planner, an_idle_machine_drives_the_fan_at_the_speed_it_was_given) {
  SimulatedMachine machine;

  thermalManager.set_fan_speed(0, 200);
  the_housekeeping_pass_runs();
  TEST_ASSERT_EQUAL_MESSAGE(200, fan_output(), "an idle machine should apply the speed it was given");

  thermalManager.set_fan_speed(0, 60);
  the_housekeeping_pass_runs();
  TEST_ASSERT_EQUAL_MESSAGE(60, fan_output(), "...and should follow it when it changes");

  thermalManager.set_fan_speed(0, 0);
  the_housekeeping_pass_runs();
}

/**
 * While printing, the fan follows the moves rather than the commands.
 *
 * A slicer puts `M106` where it wants the fan to change — after the first layer, before an
 * overhang — but by the time the firmware reads that line it has a second or two of moves
 * already planned. Applying the new speed there and then would change the fan somewhere
 * upstream of where it was asked for, and the faster the machine runs ahead of itself the
 * further out it would be.
 *
 * So the speed is recorded into each block as the move is planned, and the housekeeping pass
 * drives the fan from the block being executed. The command that arrives after a move is
 * planned does not reach the fan until the queue drains to it.
 *
 * The three speeds are the point: the fan is at 30 before, the block carries 200, the command
 * now says 60. Reading the block gives a different answer from reading the command *and* from
 * leaving the output alone, so nothing else can produce it.
 */
MARLIN_TEST(planner, a_fan_change_waits_for_the_moves_already_planned) {
  SimulatedMachine machine;

  thermalManager.set_fan_speed(0, 30);
  the_housekeeping_pass_runs();
  TEST_ASSERT_EQUAL_MESSAGE(30, fan_output(), "this test starts from a fan that is already running");

  // Plan a move while the fan is set to 200 — the block carries that speed with it.
  thermalManager.set_fan_speed(0, 200);
  xyze_pos_t to = { 0 }; to.x = 20.0f;
  TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(to, 30.0f), "the move was not accepted");

  // ...and only then does the host ask for something else.
  thermalManager.set_fan_speed(0, 60);
  the_housekeeping_pass_runs();

  TEST_ASSERT_EQUAL_MESSAGE(200, fan_output(),
    "the fan should be driven at the speed the move being executed was planned with");

  // Once the queue drains there is no move to take a speed from, and the command applies.
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the move never finished");
  the_housekeeping_pass_runs();

  TEST_ASSERT_EQUAL_MESSAGE(60, fan_output(),
    "with the queue empty the fan should catch up with what was asked for");

  thermalManager.set_fan_speed(0, 0);
  the_housekeeping_pass_runs();
}

#endif // HAS_FAN && PIN_EXISTS(PART_COOLING_FAN0)

/**
 * Each fan is driven at its own speed.
 *
 * This build has eight of them, and nothing in the firmware indexes them — every fan is a
 * separately named pin reached by a separately expanded macro, eight lines of near-identical
 * code written out by hand. That is exactly the shape a copy-paste slip hides in: a fan that
 * reads its neighbour's speed, or a line that sets the same fan twice and leaves one dark,
 * costs a print and nothing else notices.
 *
 * Eight distinct speeds, so no fan can borrow another's answer and still look right.
 */
MARLIN_TEST(planner, every_fan_is_driven_at_its_own_speed) {
  SimulatedMachine machine;

  // A fan can only be told apart from its neighbours if it has its own pin to be told apart on.
  for (uint8_t i = 0; i < FAN_COUNT; i++)
    for (uint8_t j = uint8_t(i + 1); j < FAN_COUNT; j++)
      TEST_ASSERT_TRUE_MESSAGE(FAN_PINS[i] != FAN_PINS[j], "two fans share an output pin");

  // 20, 40, 60 ... spread far enough apart that an off-by-one in the index is unmistakable.
  for (uint8_t f = 0; f < FAN_COUNT; f++) thermalManager.set_fan_speed(f, uint16_t(20 * (f + 1)));
  the_housekeeping_pass_runs();

  for (uint8_t f = 0; f < FAN_COUNT; f++) {
    char why[64];
    snprintf(why, sizeof(why), "fan %u should be driven at its own speed", unsigned(f));
    TEST_ASSERT_EQUAL_MESSAGE(20 * (f + 1), fan_output(f), why);
  }

  // ...and again in the other direction, so "fan f gets 20(f+1)" cannot be a coincidence of
  // one ascending ramp matching an index.
  for (uint8_t f = 0; f < FAN_COUNT; f++)
    thermalManager.set_fan_speed(f, uint16_t(20 * (FAN_COUNT - f)));
  the_housekeeping_pass_runs();

  for (uint8_t f = 0; f < FAN_COUNT; f++) {
    char why[64];
    snprintf(why, sizeof(why), "fan %u should follow its own speed downward too", unsigned(f));
    TEST_ASSERT_EQUAL_MESSAGE(20 * (FAN_COUNT - f), fan_output(f), why);
  }

  for (uint8_t f = 0; f < FAN_COUNT; f++) thermalManager.set_fan_speed(f, 0);
  the_housekeeping_pass_runs();
}

#if ENABLED(SLOWDOWN)

namespace {

  #ifndef SLOWDOWN_DIVISOR
    #define SLOWDOWN_DIVISOR 2
  #endif

  constexpr uint8_t SLOWDOWN_FROM = 2,
                    SLOWDOWN_UNTIL = (BLOCK_BUFFER_SIZE) / (SLOWDOWN_DIVISOR) - 1;

  constexpr float PROBE_MM = 0.1f, PROBE_MM_S = 60.0f, FILLER_MM = 5.0f;

  // Both settings the slowdown depends on, stated rather than inherited: `M205 B` outlives the
  // command that set it, and a suite that had left it at zero would find nothing slowed at any
  // depth and report the bracket as untestable rather than wrong.
  struct StatedSegmentTime {
    SimulatedMachine machine;
    uint32_t was;
    StatedSegmentTime() : was(planner.settings.min_segment_time_us) {
      planner.settings.min_segment_time_us = 20000;
    }
    ~StatedSegmentTime() {
      planner.settings.min_segment_time_us = was;
      planner.clear_block_buffer();
    }
  };

  // Put `depth` moves in the queue, then plan one short fast move on top and report the speed
  // it was given. Nothing consumes the queue, so `depth` is exactly what the planner sees.
  float speed_of_a_short_move_behind(const uint8_t depth) {
    planner.clear_block_buffer();
    xyze_pos_t at = { 0 };
    planner.set_position_mm(at);

    for (uint8_t i = 0; i < depth; i++) {
      at.x += FILLER_MM;
      TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(at, 100.0f), "a filler move was not accepted");
    }
    TEST_ASSERT_EQUAL_MESSAGE(depth, planner.movesplanned(), "the queue is not at the depth asked for");

    at.x += PROBE_MM;
    TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(at, PROBE_MM_S), "the short move was not accepted");

    const uint8_t last = (planner.block_buffer_head + BLOCK_BUFFER_SIZE - 1) % (BLOCK_BUFFER_SIZE);
    return planner.block_buffer[last].nominal_speed;
  }

  bool was_slowed(const uint8_t depth) {
    return speed_of_a_short_move_behind(depth) < PROBE_MM_S * 0.99f;
  }

}

/**
 * A short move is stretched only while the queue is shallow enough to be worth helping.
 *
 * A queue of very short segments drains faster than the host can refill it, and when it runs
 * dry the machine stops dead mid-curve. So the planner spends time it has: while the queue is
 * shallow it stretches each short segment out towards `min_segment_time_us`, buying the host
 * long enough to catch up.
 *
 * Both ends of that bracket are load-bearing and neither is obvious. Below it — a single move
 * queued — there is nothing to protect: the machine is not printing a curve, it is doing one
 * thing, and slowing it down would just be slow. Above it the queue is healthy and stretching
 * moves would cap the machine's speed for no reason at all.
 *
 * Four depths, one either side of each end, which is what says where the bracket is rather
 * than that some bracket exists.
 */
MARLIN_TEST(planner, a_short_move_is_stretched_only_while_the_queue_is_shallow) {
  StatedSegmentTime stated;

  TEST_ASSERT_FALSE_MESSAGE(was_slowed(SLOWDOWN_FROM - 1),
    "one move on its own is not a draining queue and should not be slowed");
  TEST_ASSERT_TRUE_MESSAGE(was_slowed(SLOWDOWN_FROM),
    "a queue at the shallow end of the bracket should be helped");
  TEST_ASSERT_TRUE_MESSAGE(was_slowed(SLOWDOWN_UNTIL),
    "and still at the deep end of it");
  TEST_ASSERT_FALSE_MESSAGE(was_slowed(SLOWDOWN_UNTIL + 1),
    "a queue past the bracket is keeping up and should be left alone");
}

/**
 * The deeper the queue, the less it is helped.
 *
 * The stretch is not a flat penalty: the time added is shared out by how many moves are
 * already queued, so a queue one move from empty is stretched hardest and the help tails off
 * as the host catches up. Without this, "slow down inside the bracket" and "slow down by the
 * right amount" are the same claim, and the machine would jolt back to full speed at the edge
 * of the bracket instead of easing off to meet it.
 *
 * Asserted as the relation — twice the depth, half the help — rather than as two speeds.
 */
MARLIN_TEST(planner, a_deeper_queue_is_stretched_less) {
  StatedSegmentTime stated;

  // Time taken, rather than speed, because it is the time the stretch is added to.
  const float at_2 = PROBE_MM / speed_of_a_short_move_behind(2),
              at_4 = PROBE_MM / speed_of_a_short_move_behind(4),
              unhelped = PROBE_MM / PROBE_MM_S;

  const float help_at_2 = at_2 - unhelped, help_at_4 = at_4 - unhelped;

  TEST_ASSERT_TRUE_MESSAGE(help_at_4 > 0.0f, "both depths should be inside the bracket");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f * help_at_4, help_at_4, help_at_2 / 2.0f,
    "twice as many moves queued should get half as much added");
}

#endif // SLOWDOWN
