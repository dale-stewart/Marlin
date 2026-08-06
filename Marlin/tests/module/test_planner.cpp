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
 * Tests for the motion planner's settings and conversions.
 *
 * The planner turns millimetres into steps and enforces the machine's limits. Its
 * queue cannot be exercised here — filling it blocks on a stepper interrupt that does
 * not run — but the arithmetic and the limits are what a wrong value would corrupt,
 * and those are pure.
 */

#include "../test/unit_tests.h"
#include "src/module/planner.h"
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

  struct PlainExtrusion {
    float was_e_factor;
    PlainExtrusion() { was_e_factor = planner.e_factor[0]; planner.e_factor[0] = 1.0f; }
    ~PlainExtrusion() { planner.e_factor[0] = was_e_factor; planner.clear_block_buffer(); }
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
