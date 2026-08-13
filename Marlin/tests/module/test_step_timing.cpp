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
 * The journey, not just the destination.
 *
 * tests/module/test_simulated_motion.cpp asserts where a move ends up. That says nothing
 * about *how* it got there: a move that ignored acceleration, started decelerating in the
 * wrong place, or ran at half the commanded speed still delivers the same number of
 * steps, so a final step count cannot notice any of it.
 *
 * Everything below asserts on *when* the step pulses arrive. The test HAL already makes
 * that observable without a production seam: `Gpio::attachLogger()` is part of the HAL,
 * every write to a pin carries `Clock::nanos()`, and the clock only moves when a test
 * moves it. So a move's pulse timeline is exact and repeatable — the same view a logic
 * analyser on the step pin would give.
 *
 * The claims made about that timeline are physical ones — peak speed, ramp symmetry, how
 * the move stretches when acceleration is halved — rather than claims about the variables
 * stepper.cpp happens to keep. Bands are given where the firmware's discrete arithmetic
 * cannot hit the continuous answer exactly; each one says what it is allowing for.
 *
 * Test-HAL only: under HAL/LINUX time is the wall clock and interrupts are POSIX signals,
 * so none of these timings would be reproducible.
 */


#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "src/HAL/TEST/hardware/Gpio.h"
#include "src/module/planner.h"
#include "src/module/stepper.h"

#include <vector>
#include <math.h>

namespace {

  /**
   * Every rising edge on the X step pin, in simulated nanoseconds.
   *
   * Attaching in the constructor and detaching in the destructor matters: the logger is a
   * single global hook, and leaving a dangling one behind would have the next test write
   * through a destroyed object.
   */
  class StepTimeline : public IOLogger {
  public:
    StepTimeline() { Gpio::attachLogger(this); }
    ~StepTimeline() { Gpio::attachLogger(nullptr); }

    void log(GpioEvent ev) override {
      if (ev.pin_id == X_STEP_PIN && ev.event == GpioEvent::RISE) at.push_back(ev.timestamp);
    }

    std::vector<uint64_t> at;

    size_t steps() const { return at.size(); }
    size_t gaps() const { return at.size() ? at.size() - 1 : 0; }

    // Nanoseconds between step i and step i+1.
    uint64_t gap(const size_t i) const { return at[i + 1] - at[i]; }

    // First pulse to last pulse. Shorter than the whole move by the run-up to the first
    // step and the run-out after the last, but no part of either ramp is missing.
    uint64_t span_ns() const { return at.size() < 2 ? 0 : at.back() - at.front(); }

    uint64_t shortest_gap() const {
      uint64_t m = ~uint64_t(0);
      for (size_t i = 0; i < gaps(); i++) if (gap(i) < m) m = gap(i);
      return m;
    }

    size_t index_of_shortest_gap() const {
      size_t best = 0;
      for (size_t i = 0; i < gaps(); i++) if (gap(i) < gap(best)) best = i;
      return best;
    }

    // Time from the first pulse to the fastest one, and from there to the last.
    uint64_t time_speeding_up() const { return at[index_of_shortest_gap()] - at.front(); }
    uint64_t time_slowing_down() const { return at.back() - at[index_of_shortest_gap()]; }

    // The speed a step-to-step gap represents, in mm/s, at a given resolution.
    static float mm_s(const uint64_t gap_ns, const float steps_per_mm) {
      return 1.0e9f / (float(gap_ns) * steps_per_mm);
    }

    float peak_mm_s(const float steps_per_mm = SimulatedMachine::STEPS_PER_MM) const {
      return mm_s(shortest_gap(), steps_per_mm);
    }

    // Where the cruise plateau starts and ends: the first and last gaps within `factor` of
    // the fastest. A trapezoid has a run of near-identical gaps in the middle, and
    // `index_of_shortest_gap()` picks an arbitrary one of them — which is fine for a
    // triangular move and useless for measuring the two ramps of a trapezoidal one.
    size_t index_of_first_near_fastest(const float factor) const {
      const float limit = float(shortest_gap()) * factor;
      for (size_t i = 0; i < gaps(); i++) if (float(gap(i)) <= limit) return i;
      return 0;
    }

    size_t index_of_last_near_fastest(const float factor) const {
      const float limit = float(shortest_gap()) * factor;
      size_t last = 0;
      for (size_t i = 0; i < gaps(); i++) if (float(gap(i)) <= limit) last = i;
      return last;
    }

    // Time spent getting up to the plateau, and time spent coming off it.
    uint64_t time_up_to_the_plateau(const float factor) const {
      return at[index_of_first_near_fastest(factor)] - at.front();
    }
    uint64_t time_down_from_the_plateau(const float factor) const {
      return at.back() - at[index_of_last_near_fastest(factor) + 1];
    }

    // How many gaps are within `factor` of the fastest — the cruise plateau, if there is
    // one, plus the shoulders either side of it.
    size_t gaps_near_the_fastest(const float factor) const {
      const float limit = float(shortest_gap()) * factor;
      size_t n = 0;
      for (size_t i = 0; i < gaps(); i++) if (float(gap(i)) <= limit) n++;
      return n;
    }
  };

  // Run one X move from the origin and check every step arrived, so a timeline is never
  // read from a move that silently lost steps.
  void move_x(const StepTimeline &line, const float mm, const float feedrate) {
    xyze_pos_t origin = { 0 };
    planner.set_position_mm(origin);
    xyze_pos_t target = { 0 };
    target.x = mm;
    TEST_ASSERT_TRUE(planner.buffer_line(target, feedrate));
    TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
    TEST_ASSERT_EQUAL(int32_t(planner.settings.axis_steps_per_mm[X_AXIS] * mm), stepper.position(X_AXIS));
    TEST_ASSERT_EQUAL(size_t(planner.settings.axis_steps_per_mm[X_AXIS] * mm), line.steps());
  }

  void set_acceleration(const float accel) {
    LOOP_NUM_AXES(i) planner.settings.max_acceleration_mm_per_s2[i] = uint32_t(accel);
    planner.settings.acceleration = accel;
    planner.settings.travel_acceleration = accel;
    planner.refresh_acceleration_rates();
  }

  /**
   * Re-resolve the machine: a finer microstep and a stiffer ramp.
   *
   * The fixture's 80 steps/mm is a belted axis on full steps, which asks so little of the
   * stepper interrupt that the rate-dependent arithmetic barely moves. 3200 steps/mm is an
   * ordinary 1/16-microstepped leadscrew, and at 200 mm/s it asks for 640,000 steps a
   * second, which puts the interrupt under real pressure.
   *
   * Note what this does *not* do. With `OLD_ADAPTIVE_MULTISTEPPING` disabled,
   * `steps_per_isr` is not derived from the step rate: it is a ladder, climbed one rung
   * when an interrupt overruns its own interval and given back when one finishes early. A
   * single move at this resolution stays at one step per interrupt from beginning to end,
   * whatever the feedrate. Reaching the top of the ladder needs two buffered moves — see
   * `move_x_twice` and the section at the end of this file.
   */
  void with_resolution(const float steps_per_mm, const float accel) {
    LOOP_LOGICAL_AXES(i) planner.settings.axis_steps_per_mm[i] = steps_per_mm;
    LOOP_NUM_AXES(i) planner.settings.max_feedrate_mm_s[i] = 1000.0f;
    planner.refresh_positioning();
    set_acceleration(accel);
  }

  /**
   * A train of short collinear moves, all buffered before any of them runs.
   *
   * This is the shape of dense G-code — a curve rendered as many tiny segments — and it is
   * the only way to reach the planner's buffer-depth arithmetic. Buffering is what sets the
   * state that arithmetic reads: each move is planned while the ones before it are still
   * waiting, so the queue depth it sees is its own position in the train.
   *
   * Collinear and in one direction so the planner carries speed straight through, which is
   * what lets each block actually reach its own nominal rate instead of being limited by the
   * ramp into it. A single short move cannot do this: it starts and ends at rest, so
   * acceleration decides its duration and the nominal rate is never reached.
   */
  void buffer_train(const size_t moves, const float mm, const float feedrate) {
    xyze_pos_t at = { 0 };
    planner.set_position_mm(at);
    for (size_t i = 1; i <= moves; i++) {
      at.x = float(i) * mm;
      TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(at, feedrate), "the train did not fit in the buffer");
    }
  }

  // How long the nth move of such a train took, in microseconds, from a timeline of the
  // whole run. Moves are equal length, so each occupies a known run of steps.
  float move_us(const StepTimeline &line, const size_t n, const size_t steps_per_move) {
    const size_t from = (n - 1) * steps_per_move, to = n * steps_per_move;
    TEST_ASSERT_TRUE_MESSAGE(to < line.steps(), "the timeline is shorter than the move asked about");
    return float(line.at[to] - line.at[from]) / 1000.0f;
  }

  /**
   * A circle rendered as chords, buffered whole.
   *
   * A slicer emits curves this way, and it is the case the planner's small-segment junction
   * handling exists for: each corner is a small direction change between two short moves, and
   * taken naively the junction-deviation formula allows an absurd speed through it because
   * the turn is so slight.
   *
   * The chord length and the turn angle both follow from the radius and the segment count, so
   * a test can state the arc the machine is being asked to follow rather than a list of
   * points.
   */
  void buffer_circle(const float radius, const size_t segments, const float feedrate) {
    xyze_pos_t at = { 0 };
    at.x = radius;
    planner.set_position_mm(at);
    for (size_t i = 1; i <= segments; i++) {
      const float a = 2.0f * float(M_PI) * float(i) / float(segments);
      at.x = radius * cosf(a);
      at.y = radius * sinf(a);
      TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(at, feedrate), "the circle did not fit in the buffer");
    }
  }

  struct SavedMinSegmentTime {
    uint32_t was;
    SavedMinSegmentTime() : was(planner.settings.min_segment_time_us) {}
    ~SavedMinSegmentTime() { planner.settings.min_segment_time_us = was; }
  };

  // Ratio of two durations, largest first, for assertions written as "x times longer".
  float ratio(const uint64_t a, const uint64_t b) { return float(a) / float(b); }

  /**
   * Two moves in a row, buffered before either runs, so the second inherits the first's
   * speed instead of starting from rest. Both are along X and in the same direction, so
   * the planner carries the junction speed straight through.
   *
   * A single move cannot reach the top of the multistepping ladder and then run somewhere
   * useful: the stepper only climbs the ladder while it is falling behind, and by the time
   * it is no longer falling behind the move is over. Handing it a fast move followed by a
   * slower one puts it at the top of the ladder *and* gives it a steady rate to hold it
   * against.
   */
  void move_x_twice(const StepTimeline &line,
                    const float mm1, const float feedrate1,
                    const float mm2, const float feedrate2) {
    xyze_pos_t origin = { 0 };
    planner.set_position_mm(origin);
    xyze_pos_t first = { 0 };  first.x  = mm1;
    xyze_pos_t second = { 0 }; second.x = mm1 + mm2;
    TEST_ASSERT_TRUE(planner.buffer_line(first, feedrate1));
    TEST_ASSERT_TRUE(planner.buffer_line(second, feedrate2));
    TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
    const float total = mm1 + mm2;
    TEST_ASSERT_EQUAL(int32_t(planner.settings.axis_steps_per_mm[X_AXIS] * total), stepper.position(X_AXIS));
    TEST_ASSERT_EQUAL(size_t(planner.settings.axis_steps_per_mm[X_AXIS] * total), line.steps());
  }

  /**
   * The pulse timeline seen as interrupts rather than as steps.
   *
   * Pulses issued inside one interrupt arrive back to back — as close together as the
   * pulse timing allows — while the next interrupt is a scheduled interval away. So a gap
   * several times the shortest one in the window is an interrupt boundary, and everything
   * between two boundaries was delivered by a single interrupt.
   *
   * That makes both halves of multistepping visible from the step pin alone: how many
   * pulses an interrupt delivered, and how long the stepper then waited before the next.
   */
  struct Interrupt {
    size_t group;         // pulses this interrupt delivered
    uint64_t interval_ns; // the wait that preceded it, from the previous group's last pulse
  };

  struct Interrupts {
    size_t group;         // pulses delivered per interrupt; 0 if the groups are not uniform
    uint64_t interval_ns; // last pulse of one group to the first pulse of the next
    size_t count;         // complete groups measured

    // `from` and `to` are fractions of the move, so a window can be placed in the cruise
    // without knowing the step count. Groups partly outside the window are dropped.
    static std::vector<Interrupt> list(const StepTimeline &line, const float from, const float to) {
      const size_t a = size_t(float(line.steps()) * from), b = size_t(float(line.steps()) * to);
      uint64_t tightest = ~uint64_t(0);
      for (size_t i = a; i + 1 < b; i++) tightest = _MIN(tightest, line.at[i + 1] - line.at[i]);

      const uint64_t boundary = tightest * 4;
      std::vector<Interrupt> out;
      size_t size = 0;
      bool started = false;
      uint64_t pending_interval = 0;

      for (size_t i = a; i + 1 < b; i++) {
        const uint64_t g = line.at[i + 1] - line.at[i];
        if (g <= boundary) { size++; continue; }
        // A boundary closes the group that was being counted.
        if (started) out.push_back({ size + 1, pending_interval });
        started = true;
        size = 0;
        pending_interval = g;
      }

      // No boundary anywhere means one pulse per interrupt at a steady interval.
      if (!started) for (size_t i = a; i + 1 < b; i++) out.push_back({ 1, line.at[i + 1] - line.at[i] });
      return out;
    }

    static Interrupts over(const StepTimeline &line, const float from, const float to) {
      const std::vector<Interrupt> all = list(line, from, to);
      Interrupts r = { 0, 0, all.size() };
      if (all.empty()) return r;

      uint64_t total = 0;
      bool uniform = true;
      for (const Interrupt &i : all) { total += i.interval_ns; if (i.group != all[0].group) uniform = false; }

      r.group = uniform ? all[0].group : 0;
      r.interval_ns = total / all.size();
      return r;
    }
  };

  // The time the given number of steps takes at a commanded feedrate, in nanoseconds.
  float steps_ns(const size_t steps, const float feedrate, const float steps_per_mm) {
    return 1.0e9f * float(steps) / (feedrate * steps_per_mm);
  }

}

//
// ---- The velocity profile ----
//

/**
 * A move speeds up, then slows down, and never does either in the wrong direction.
 *
 * 10 mm at 3000 mm/s² cannot reach 300 mm/s in the distance available, so the move is a
 * pure triangle: every step to the middle is closer to its neighbour than the one before,
 * and every step after it is further apart. That single shape is what `accelerate_before`,
 * `decelerate_start` and the acceleration/deceleration clocks exist to produce, and a
 * final step count cannot see any of it.
 */
MARLIN_TEST(step_timing, a_move_speeds_up_and_then_slows_down) {
  SimulatedMachine machine;
  StepTimeline line;
  move_x(line, 10.0f, 300.0f);

  const size_t fastest = line.index_of_shortest_gap();

  // Not flat: the ends are far slower than the middle.
  TEST_ASSERT_TRUE(line.gap(0) > line.shortest_gap() * 8);
  TEST_ASSERT_TRUE(line.gap(line.gaps() - 1) > line.shortest_gap() * 8);

  // Monotonic up to the fastest step and monotonic away from it afterwards.
  for (size_t i = 1; i <= fastest; i++)
    TEST_ASSERT_TRUE(line.gap(i) <= line.gap(i - 1));
  for (size_t i = fastest + 1; i < line.gaps(); i++)
    TEST_ASSERT_TRUE(line.gap(i) >= line.gap(i - 1));
}

/**
 * The peak of that triangle is where the ramp puts it: halfway.
 *
 * A move that accelerates and decelerates at the same rate reaches its top speed at the
 * midpoint. Getting `decelerate_start` wrong moves the peak without changing the step
 * count.
 */
MARLIN_TEST(step_timing, a_triangular_move_peaks_at_its_midpoint) {
  SimulatedMachine machine;
  StepTimeline line;
  move_x(line, 10.0f, 300.0f);

  const float where = float(line.index_of_shortest_gap()) / float(line.gaps());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.5f, where);
}

/**
 * Speeding up and slowing down take the same length of time.
 *
 * The two halves are computed by different code — `acceleration_time` counts up from an
 * initial rate, `deceleration_time` counts down from the cruise rate — so a symmetric
 * move is the test that says they agree. The 5% band allows for the two clocks starting
 * half an interval apart, which is deliberate (`acceleration_time = deceleration_time =
 * interval / 2`).
 */
MARLIN_TEST(step_timing, acceleration_and_deceleration_take_the_same_time) {
  SimulatedMachine machine;
  StepTimeline line;
  move_x(line, 10.0f, 300.0f);

  const uint64_t up = line.time_speeding_up(), down = line.time_slowing_down();
  TEST_ASSERT_TRUE(up > 0 && down > 0);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, ratio(up, down));
}

/**
 * The top speed is the one the ramp allows, not the one that was asked for.
 *
 * Accelerating at `a` over half of a distance `d` and decelerating over the other half
 * peaks at sqrt(a·d) — here sqrt(3000 × 10) = 173.2 mm/s, well below the 300 mm/s
 * requested. The 3% band is the firmware's integer step-rate arithmetic; the value is
 * physics, not a recorded measurement.
 */
MARLIN_TEST(step_timing, an_acceleration_limited_move_peaks_where_the_ramp_allows) {
  SimulatedMachine machine;
  StepTimeline line;
  move_x(line, 10.0f, 300.0f);

  const float predicted = sqrtf(3000.0f * 10.0f);
  TEST_ASSERT_FLOAT_WITHIN(predicted * 0.03f, predicted, line.peak_mm_s());
}

/**
 * Halving the acceleration stretches the move by root two and lowers its peak by root two.
 *
 * For a triangular move t = 2·sqrt(d/a) and v = sqrt(a·d), so both scale as sqrt(a). This
 * is the assertion that says the acceleration *rate* is actually being applied rather than
 * some fixed ramp: it is a ratio, so every fixed overhead in the measurement cancels, and
 * the band can be tight.
 */
MARLIN_TEST(step_timing, halving_the_acceleration_stretches_the_move_by_root_two) {
  SimulatedMachine machine;

  uint64_t span_fast, span_slow;
  float peak_fast, peak_slow;

  {
    StepTimeline line;
    set_acceleration(3000.0f);
    move_x(line, 10.0f, 300.0f);
    span_fast = line.span_ns();
    peak_fast = line.peak_mm_s();
  }
  {
    StepTimeline line;
    set_acceleration(1500.0f);
    move_x(line, 10.0f, 300.0f);
    span_slow = line.span_ns();
    peak_slow = line.peak_mm_s();
  }

  TEST_ASSERT_FLOAT_WITHIN(0.02f, sqrtf(2.0f), ratio(span_slow, span_fast));
  TEST_ASSERT_FLOAT_WITHIN(0.02f, sqrtf(2.0f), peak_fast / peak_slow);
}

/**
 * A move long enough to get up to speed cruises there, at the speed it was given.
 *
 * 10 mm at 50 mm/s needs 0.4 mm of ramp at each end, so the middle is a plateau — the
 * cruise branch, which computes one interval (`ticks_nominal`) and reuses it. Both halves
 * of that matter: the plateau exists, and it is at 50 mm/s.
 */
MARLIN_TEST(step_timing, a_long_move_cruises_at_the_commanded_feedrate) {
  SimulatedMachine machine;
  StepTimeline line;
  move_x(line, 10.0f, 50.0f);

  TEST_ASSERT_FLOAT_WITHIN(0.5f, 50.0f, line.peak_mm_s());

  // Most of the move is at that speed, rather than the peak being a single instant.
  TEST_ASSERT_TRUE(line.gaps_near_the_fastest(1.001f) > (line.gaps() * 85) / 100);
}

/**
 * The cruise really is a constant interval, not a slow drift.
 *
 * Taken across the middle half of the move, where neither ramp reaches, every gap is the
 * same. A profile that kept adjusting the rate while cruising would still average 50 mm/s
 * and still deliver 800 steps.
 */
MARLIN_TEST(step_timing, the_cruise_interval_does_not_drift) {
  SimulatedMachine machine;
  StepTimeline line;
  move_x(line, 10.0f, 50.0f);

  const size_t from = line.gaps() / 4, to = (line.gaps() * 3) / 4;
  for (size_t i = from; i < to; i++)
    TEST_ASSERT_EQUAL_UINT64(line.gap(from), line.gap(i));
}

/**
 * A move too slow to need a ramp runs at one interval from beginning to end.
 *
 * 5 mm/s is reached in under two milliseconds, which is less than a single step interval
 * at 80 steps/mm, so there is no observable acceleration phase at all. Every gap but the
 * last — which absorbs the end of the block — is identical.
 */
/**
 * A move that cruises comes off the plateau as it went on.
 *
 * The two ramps of a trapezoid are the same ramp: same acceleration, same speed range, so
 * the same duration. `acceleration_and_deceleration_take_the_same_time` says this of a
 * *triangular* move, where the peak is a single point. A cruising move takes a different
 * path through the interrupt — deceleration is set up during the cruise phase rather than
 * carried straight over from acceleration, and its timer is seeded there — so the symmetry
 * has to be asserted again on this side of that branch.
 *
 * The plateau is found by its gaps rather than by its single fastest one, because a
 * trapezoid has a run of near-identical gaps and any of them could be the shortest.
 */
MARLIN_TEST(step_timing, a_cruising_move_comes_off_the_plateau_as_it_went_on) {
  SimulatedMachine machine;
  set_acceleration(3000.0f);

  StepTimeline line;
  move_x(line, 10.0f, 50.0f);

  // Within 2% of the fastest gap is the plateau: the ramps either side of it change speed
  // far faster than that between one step and the next.
  constexpr float PLATEAU = 1.02f;
  TEST_ASSERT_TRUE_MESSAGE(line.gaps_near_the_fastest(PLATEAU) > line.gaps() / 2,
    "this move is meant to spend most of its length cruising");

  const uint64_t up = line.time_up_to_the_plateau(PLATEAU),
                 down = line.time_down_from_the_plateau(PLATEAU);

  TEST_ASSERT_TRUE_MESSAGE(up > 0 && down > 0, "both ramps should take a measurable time");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.15f, 1.0f, ratio(up, down),
    "the two ramps of a trapezoid are the same ramp and should take the same time");
}

/**
 * A cruising move takes as long as its trapezoid says it should.
 *
 * Predicted from the acceleration and the feedrate, not from a previous run: two ramps of
 * `v/a` seconds covering `v^2/a` millimetres between them, and the rest at `v`. Getting the
 * deceleration phase started wrongly shows up here as a duration that no longer matches the
 * shape, in a way that the symmetry above would not notice if both ramps were wrong together.
 */
MARLIN_TEST(step_timing, a_cruising_move_takes_as_long_as_its_trapezoid) {
  SimulatedMachine machine;
  constexpr float ACCEL = 3000.0f, FEEDRATE = 50.0f, DISTANCE = 10.0f;
  set_acceleration(ACCEL);

  StepTimeline line;
  move_x(line, DISTANCE, FEEDRATE);

  const float ramp_s = FEEDRATE / ACCEL,
              ramp_mm = FEEDRATE * FEEDRATE / ACCEL,     // both ramps together
              cruise_s = (DISTANCE - ramp_mm) / FEEDRATE;
  TEST_ASSERT_TRUE_MESSAGE(cruise_s > 0.0f, "this move must be long enough to reach the feedrate");

  const float predicted_s = 2.0f * ramp_s + cruise_s;
  const float measured_s = float(line.span_ns()) / 1.0e9f;

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(predicted_s * 0.05f, predicted_s, measured_s,
    "a trapezoidal move should take two ramps plus a cruise");
}

MARLIN_TEST(step_timing, a_slow_move_never_ramps) {
  SimulatedMachine machine;
  StepTimeline line;
  move_x(line, 2.0f, 5.0f);

  TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.0f, line.peak_mm_s());
  for (size_t i = 1; i + 1 < line.gaps(); i++)
    TEST_ASSERT_EQUAL_UINT64(line.gap(0), line.gap(i));
}

/**
 * At a constant speed, twice the distance takes twice as long.
 *
 * The trivial physical claim, and the one that catches a profile that is internally
 * consistent but scaled wrongly: both moves would still land on an exact step count.
 */
MARLIN_TEST(step_timing, twice_the_distance_at_one_speed_takes_twice_the_time) {
  SimulatedMachine machine;

  uint64_t span_short, span_long;
  { StepTimeline line; move_x(line, 1.0f, 5.0f); span_short = line.span_ns(); }
  { StepTimeline line; move_x(line, 2.0f, 5.0f); span_long  = line.span_ns(); }

  TEST_ASSERT_FLOAT_WITHIN(0.02f, 2.0f, ratio(span_long, span_short));
}

//
// ---- Multistepping ----
//

/**
 * A step rate higher than the interrupt can serve is delivered several steps at a time.
 *
 * At 3200 steps/mm — an ordinary 1/16-microstepped leadscrew — 200 mm/s is 640,000 steps
 * a second. One interrupt per step is not on offer at that rate, so the stepper emits
 * more than one pulse per interrupt and divides the interval to match. Nothing in the
 * existing suite goes fast enough to enter that path: at the fixture's 80 steps/mm the
 * fastest move the machine will accept is under 25,000 steps a second.
 *
 * The evidence is in the pulse spacing. Doubling the feedrate from 100 to 200 mm/s can at
 * best halve the interval between interrupts, so a one-pulse-per-interrupt driver could
 * not put pulses closer than half of the 100 mm/s spacing. Anything much closer than that
 * is two pulses issued back to back inside one interrupt.
 */
MARLIN_TEST(step_timing, a_step_rate_beyond_one_step_per_interrupt_doubles_up_the_pulses) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 20000.0f);

  uint64_t closest_at_100, closest_at_200;
  { StepTimeline line; move_x(line, 5.0f, 100.0f); closest_at_100 = line.shortest_gap(); }
  { StepTimeline line; move_x(line, 5.0f, 200.0f); closest_at_200 = line.shortest_gap(); }

  // Twice the feedrate, but far more than twice as close together.
  TEST_ASSERT_TRUE(closest_at_200 * 3 < closest_at_100);
}

/**
 * A high step rate loses no steps and gains none.
 *
 * The distance a move covers is fixed by its step count, not by how fast the interrupt
 * managed to issue them, so an error in the interval arithmetic must not leak into the
 * position. `move_x` checks the step count and the stepper's own position on every move,
 * and this runs it at three feedrates a factor of four apart: the same 5 mm arrives at
 * 16,000 steps at every one of them.
 */
MARLIN_TEST(step_timing, a_high_step_rate_delivers_exactly_the_steps_asked_for) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 20000.0f);

  for (const float feedrate : { 50.0f, 100.0f, 200.0f }) {
    StepTimeline line;
    move_x(line, 5.0f, feedrate);
  }
}

/**
 * A move at a high step rate still takes about the time the feedrate implies.
 *
 * The bounds are computed from the ramp, not read off a previous run. 5 mm at 200 mm/s
 * and 20,000 mm/s² is 1 mm of ramp at each end and 3 mm of cruise: 20 ms of ramp plus
 * 15 ms of cruise, 35 ms in all. That is a floor, because the firmware must never exceed
 * the commanded feedrate. The ceiling says the interval arithmetic is not costing the
 * move a factor — getting a shift wrong by one bit halves or doubles the effective rate,
 * which neither bound would allow.
 */
MARLIN_TEST(step_timing, a_high_step_rate_keeps_to_the_commanded_feedrate) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 20000.0f);

  StepTimeline line;
  move_x(line, 5.0f, 200.0f);

  const uint64_t ideal_ns = 35000000;   // 2 x 200/20000 s of ramp + 3/200 s of cruise
  TEST_ASSERT_TRUE(line.span_ns() >= ideal_ns);
  TEST_ASSERT_TRUE(line.span_ns() < ideal_ns * 2);
}

/**
 * Asking for four times the feedrate buys most of four times the speed.
 *
 * Both bounds follow from the commanded rates rather than from a recorded duration. Four
 * times the feedrate can never be more than four times the speed over the same distance —
 * that is all the move was asked for — and it must be at least twice, because the fixed
 * ramp at each end is the only thing the extra speed has to pay for. A rate that saturated
 * somewhere in between would fail the lower bound.
 */
MARLIN_TEST(step_timing, four_times_the_feedrate_is_most_of_four_times_the_speed) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 20000.0f);

  uint64_t span_50, span_200;
  { StepTimeline line; move_x(line, 5.0f, 50.0f);  span_50  = line.span_ns(); }
  { StepTimeline line; move_x(line, 5.0f, 200.0f); span_200 = line.span_ns(); }

  TEST_ASSERT_TRUE(ratio(span_50, span_200) >= 2.0f);
  TEST_ASSERT_TRUE(ratio(span_50, span_200) <= 4.0f);
}

//
// ---- The top of the multistepping ladder ----
//
// Everything above reaches at most one pulse per interrupt: the stepper only climbs the
// multistepping ladder while an interrupt is overrunning its own interval, and a single
// move that overruns is over before it settles anywhere. A fast move followed by a slower
// one leaves it at the top of the ladder with a steady rate to hold it there, which is the
// only way the >= 16 arm of `calc_multistep_timer_interval` runs at all.
//

/**
 * At the top of the ladder an interrupt delivers a whole batch of pulses.
 *
 * `MULTISTEPPING_LIMIT` is the most steps the firmware will issue from one interrupt. 5 mm
 * at 800 mm/s asks for 2.56 million steps a second, far more than the interrupt can serve
 * one at a time, so the stepper climbs to the limit; the 200 mm/s move that follows is
 * slow enough to keep up with at that batch size and fast enough not to give it back.
 *
 * The claim is about the shape of the timeline, not about a variable: sixteen pulses
 * arrive together, then nothing for an interval, then sixteen more — uniformly, right
 * across the middle of the second move.
 */
MARLIN_TEST(step_timing, a_saturated_move_climbs_to_the_multistepping_limit) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 50000.0f);

  StepTimeline line;
  move_x_twice(line, 5.0f, 800.0f, 5.0f, 200.0f);

  const Interrupts isr = Interrupts::over(line, 0.60f, 0.80f);
  TEST_ASSERT_EQUAL(size_t(MULTISTEPPING_LIMIT), isr.group);
  TEST_ASSERT_TRUE(isr.count > 100);   // a sustained plateau, not one lucky interrupt
}

/**
 * An interrupt that delivers sixteen steps waits for sixteen steps' worth of time.
 *
 * That is the whole point of the divisor: a batch of `n` covers `n` steps of the move, so
 * the next interrupt is due when those `n` steps would have been due at the commanded
 * rate. Sixteen steps at 200 mm/s and 3200 steps/mm is 25 µs.
 *
 * 120 mm/s exercises the same law one rung down. The stepper gives a rung back while the
 * first move is winding down and then holds at eight for the whole of the second — which
 * is a different arm of the arithmetic, shifting by two and then by one instead of by
 * four — so eight steps' worth of time is 20.8 µs. The expected batch size is stated
 * rather than read back from the measurement, because a divisor that was wrong would
 * settle the ladder somewhere else and a self-derived prediction would follow it there.
 *
 * Both are measured a little long by the same fixed amount, because the interrupt reads
 * the timer a fixed number of times and each read costs the simulated CPU a tick. The 8%
 * band covers that; getting the divisor wrong by a single bit doubles or halves the
 * answer, which it does not cover.
 */
MARLIN_TEST(step_timing, a_multistepped_interrupt_waits_for_the_steps_it_delivered) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 50000.0f);

  struct { float feedrate; size_t group; } expected[] = { { 200.0f, 16 }, { 120.0f, 8 } };

  for (const auto &e : expected) {
    StepTimeline line;
    move_x_twice(line, 5.0f, 800.0f, 5.0f, e.feedrate);

    const Interrupts isr = Interrupts::over(line, 0.60f, 0.80f);
    TEST_ASSERT_EQUAL(e.group, isr.group);

    const float predicted = steps_ns(e.group, e.feedrate, 3200.0f);
    TEST_ASSERT_FLOAT_WITHIN(predicted * 0.08f, predicted, float(isr.interval_ns));
  }
}

/**
 * Change the commanded rate and the interval changes by exactly the difference.
 *
 * Both moves batch sixteen pulses, so both carry the same fixed measurement overhead and
 * subtracting one from the other removes it. What is left is pure physics — sixteen steps
 * at 150 mm/s takes 8.33 µs longer than sixteen steps at 200 mm/s — and it holds to within
 * 2%, which no change to the shift could survive.
 */
MARLIN_TEST(step_timing, the_multistepped_interval_follows_the_commanded_rate) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 50000.0f);

  Interrupts fast, slow;
  { StepTimeline line; move_x_twice(line, 5.0f, 800.0f, 5.0f, 200.0f); fast = Interrupts::over(line, 0.60f, 0.80f); }
  { StepTimeline line; move_x_twice(line, 5.0f, 800.0f, 5.0f, 150.0f); slow = Interrupts::over(line, 0.60f, 0.80f); }

  TEST_ASSERT_EQUAL(size_t(MULTISTEPPING_LIMIT), fast.group);
  TEST_ASSERT_EQUAL(size_t(MULTISTEPPING_LIMIT), slow.group);

  const float predicted = steps_ns(MULTISTEPPING_LIMIT, 150.0f, 3200.0f)
                        - steps_ns(MULTISTEPPING_LIMIT, 200.0f, 3200.0f);
  const float measured = float(slow.interval_ns) - float(fast.interval_ns);
  TEST_ASSERT_FLOAT_WITHIN(predicted * 0.02f, predicted, measured);
}

/**
 * Multistepping is given back once it is no longer needed.
 *
 * The ladder is climbed when an interrupt overruns and descended when it finds itself
 * waiting — so a move slow enough to serve one step at a time must end up doing exactly
 * that, however fast the move before it was. Without the descent the stepper would keep
 * batching sixteen pulses at a time forever after one fast move, which is visible as
 * pulses arriving in clumps rather than evenly spaced.
 *
 * Two slow moves rather than one, so the interval can be checked the same way as the
 * multistepped ones: the difference between them is the difference the commanded rates
 * imply, with the per-interrupt overhead cancelling out.
 */
MARLIN_TEST(step_timing, multistepping_is_given_back_when_the_move_slows_down) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 50000.0f);

  Interrupts at_60, at_40;
  { StepTimeline line; move_x_twice(line, 5.0f, 800.0f, 5.0f, 60.0f); at_60 = Interrupts::over(line, 0.60f, 0.80f); }
  { StepTimeline line; move_x_twice(line, 5.0f, 800.0f, 5.0f, 40.0f); at_40 = Interrupts::over(line, 0.60f, 0.80f); }

  // Back to one pulse per interrupt, evenly spaced, however fast the move before was.
  TEST_ASSERT_EQUAL(size_t(1), at_60.group);
  TEST_ASSERT_EQUAL(size_t(1), at_40.group);

  const float predicted = steps_ns(1, 40.0f, 3200.0f) - steps_ns(1, 60.0f, 3200.0f);
  const float measured = float(at_40.interval_ns) - float(at_60.interval_ns);
  TEST_ASSERT_FLOAT_WITHIN(predicted * 0.02f, predicted, measured);
}

/**
 * Coming down the ladder, each rung waits for exactly the steps it delivers.
 *
 * The rungs above are each measured on their own plateau, at a rate chosen to hold the
 * stepper there. This one catches it mid-descent instead: a stiff enough ramp finishes the
 * first move before the ladder has finished unwinding, so the second move is already
 * cruising at a fixed 320,000 steps a second while the stepper is still handing rungs
 * back — sixteen steps at a time, then eight, then four, then two, then one.
 *
 * One rate, four batch sizes, one law: the wait before an interrupt is the time its batch
 * covers, plus the fixed cost of running an interrupt at all. Measuring four rungs at one
 * rate separates those two terms, which no single measurement can — the part that scales
 * with the batch is the arithmetic under test, and what is left over must be the same
 * however big the batch was. Any error in the shift changes one rung's share and not the
 * next one's, so the leftovers stop agreeing.
 *
 * It also puts the descent inside a *cruise*, which the plateau tests do not: the cruise
 * interval is worked out once and cached, so a rung handed back while cruising is only
 * honoured if the cached value is thrown away with it.
 */
MARLIN_TEST(step_timing, every_rung_of_the_ladder_waits_for_the_steps_it_delivers) {
  SimulatedMachine machine;
  with_resolution(3200.0f, 200000.0f);

  StepTimeline line;
  move_x_twice(line, 5.0f, 800.0f, 5.0f, 100.0f);

  // The first interrupts of the second move, which begins already at cruise speed.
  const std::vector<Interrupt> isr = Interrupts::list(line, 0.50f, 0.52f);

  const size_t rungs[] = { 8, 4, 2 };
  size_t rung = 0;
  float overhead[COUNT(rungs)] = { 0 };

  for (const Interrupt &i : isr) {
    if (rung < COUNT(rungs) && i.group == rungs[rung]) {
      overhead[rung] = float(i.interval_ns) - steps_ns(rungs[rung], 100.0f, 3200.0f);
      rung++;
    }
  }

  // Eight, then four, then two, in that order, and one step at a time by the end.
  TEST_ASSERT_EQUAL(COUNT(rungs), rung);
  TEST_ASSERT_EQUAL(size_t(1), isr.back().group);

  // What each interrupt cost over and above the steps it covered, the same every time.
  for (size_t i = 0; i < COUNT(rungs); i++) {
    TEST_ASSERT_FLOAT_WITHIN(100.0f, overhead[0], overhead[i]);
    // ...and small enough that the batch, not the overhead, is what was measured.
    TEST_ASSERT_TRUE(overhead[i] > 0.0f && overhead[i] < steps_ns(1, 100.0f, 3200.0f));
  }
}

//
// ---- Slowing down for a draining buffer ----
//

/**
 * When the buffer is nearly empty the planner stretches short moves to refill it.
 *
 * A machine fed segments faster than it can plan them empties its buffer and then stutters,
 * because each stop-start is a full ramp. `SLOWDOWN` prevents that by slowing the moves down:
 * a segment that would take less than `min_segment_time_us` is stretched, and stretched more
 * the emptier the buffer is.
 *
 * The arithmetic has an exact case. With two moves queued the formula
 * `segment + 2 * (minimum - segment) / queued` reduces to the minimum itself, so the third
 * move of a freshly buffered train should take exactly `min_segment_time_us` — a prediction
 * from the configuration, not a number read off a run.
 *
 * Nothing in the suite reached this before. A single short fast move never gets there: it
 * starts and ends at rest, so acceleration decides its duration and the nominal rate the
 * stretch modifies is never reached. It takes a train.
 */
MARLIN_TEST(step_timing, a_draining_buffer_stretches_a_short_move_to_the_minimum) {
  SimulatedMachine machine;
  SavedMinSegmentTime restore;
  set_acceleration(3000.0f);

  constexpr float MM = 0.5f, FEEDRATE = 300.0f;
  const size_t steps_per_move = size_t(MM * SimulatedMachine::STEPS_PER_MM);
  const float natural_us = 1.0e6f * MM / FEEDRATE;

  planner.settings.min_segment_time_us = 20000;
  TEST_ASSERT_TRUE_MESSAGE(natural_us < float(planner.settings.min_segment_time_us),
    "these moves must be quicker than the minimum, or there is nothing to stretch");

  StepTimeline line;
  buffer_train(6, MM, FEEDRATE);
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the train never finished");

  // The third move is the one planned with exactly two ahead of it.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(float(planner.settings.min_segment_time_us) * 0.15f,
    float(planner.settings.min_segment_time_us), move_us(line, 3, steps_per_move),
    "with two moves queued the stretch should reach exactly the minimum segment time");
}

/**
 * The stretch shrinks as the buffer refills.
 *
 * The correction is `2 * (minimum - segment) / queued`, so it is halved when twice as much
 * work is waiting — the planner intervenes hardest when the machine is closest to running
 * dry and backs off as it recovers. Asserting one stretched move says only that some
 * slowdown happened; asserting that later moves are stretched *less* says it is a response
 * to the buffer depth.
 */
MARLIN_TEST(step_timing, the_stretch_eases_off_as_the_buffer_refills) {
  SimulatedMachine machine;
  SavedMinSegmentTime restore;
  set_acceleration(3000.0f);

  constexpr float MM = 0.5f, FEEDRATE = 300.0f;
  const size_t steps_per_move = size_t(MM * SimulatedMachine::STEPS_PER_MM);
  planner.settings.min_segment_time_us = 20000;

  StepTimeline line;
  buffer_train(7, MM, FEEDRATE);
  TEST_ASSERT_TRUE_MESSAGE(SimulatedMachine::run_until_idle(), "the train never finished");

  const float third = move_us(line, 3, steps_per_move),
              fourth = move_us(line, 4, steps_per_move),
              fifth = move_us(line, 5, steps_per_move);

  TEST_ASSERT_TRUE_MESSAGE(third > fourth,
    "the move planned with two ahead of it should be stretched more than the one with three");
  TEST_ASSERT_TRUE_MESSAGE(fourth > fifth,
    "and that one more than the next");
}

/**
 * With no minimum set, nothing is stretched.
 *
 * The control for both tests above: the same train, the same buffer depths, and the only
 * difference is the setting under test. `M205 B0` is how an operator turns this off, and a
 * machine that slowed down anyway would be ignoring them.
 */
MARLIN_TEST(step_timing, no_minimum_segment_time_means_no_slowdown) {
  SimulatedMachine machine;
  SavedMinSegmentTime restore;
  set_acceleration(3000.0f);

  constexpr float MM = 0.5f, FEEDRATE = 300.0f;
  const size_t steps_per_move = size_t(MM * SimulatedMachine::STEPS_PER_MM);

  float stretched, unstretched;
  {
    planner.settings.min_segment_time_us = 20000;
    StepTimeline line;
    buffer_train(6, MM, FEEDRATE);
    TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
    stretched = move_us(line, 3, steps_per_move);
  }
  {
    planner.settings.min_segment_time_us = 0;
    StepTimeline line;
    buffer_train(6, MM, FEEDRATE);
    TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
    unstretched = move_us(line, 3, steps_per_move);
  }

  TEST_ASSERT_TRUE_MESSAGE(stretched > unstretched * 2.0f,
    "asking for no minimum segment time should leave the move far quicker than the stretched one");
}

//
// ---- Cornering on a curve made of short segments ----
//

/**
 * A curve made of short segments is cornered, not taken at the commanded feedrate.
 *
 * A slicer emits curves as chords, and each corner is a small direction change between two
 * short moves. The planner limits the speed through them — otherwise a small circle would be
 * attempted at whatever feedrate the file asked for and the machine would shake itself apart.
 *
 * What this does **not** test is the small-segment arc cap at `planner.cpp:2594`, which was
 * the intention. That cap is reachable and it is masked: junction deviation depends only on
 * the corner *angle*, and in this configuration its limit is the lower of the two for every
 * radius the machine can actually get round. Compiling the branch out changes nothing
 * measurable here — checked by hand, which is the only reason this comment is not claiming
 * otherwise. See docs/defect-register.md #29.
 */
MARLIN_TEST(step_timing, a_small_circle_is_taken_at_the_speed_its_radius_allows) {
  SimulatedMachine machine;
  constexpr float ACCEL = 3000.0f;
  set_acceleration(ACCEL);

  // Fifteen chords is a 24° turn at each corner: well inside the 45° the small-segment
  // handling applies to, and short enough at these radii to stay under a millimetre.
  constexpr size_t SEGMENTS = 15;
  constexpr float SMALL_R = 0.55f, LARGE_R = 4.0f * SMALL_R, FEEDRATE = 300.0f;

  const float chord = 2.0f * LARGE_R * sinf(float(M_PI) / float(SEGMENTS));
  TEST_ASSERT_TRUE_MESSAGE(chord < 1.0f,
    "both circles must be made of segments under a millimetre to reach this path");

  float slow, fast;
  { StepTimeline line; buffer_circle(SMALL_R, SEGMENTS, FEEDRATE);
    TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
    slow = line.peak_mm_s(); }
  { StepTimeline line; buffer_circle(LARGE_R, SEGMENTS, FEEDRATE);
    TEST_ASSERT_TRUE(SimulatedMachine::run_until_idle());
    fast = line.peak_mm_s(); }

  // Both must be limited by the corners rather than by the commanded feedrate, or this
  // measures the feedrate twice.
  TEST_ASSERT_TRUE_MESSAGE(fast < FEEDRATE * 0.8f,
    "the corner limit should be what holds these circles back, not the feedrate");

  // Neither is taken faster than an arc of its own radius would allow. That bound holds
  // whichever of the two limits is binding, so it says the machine is cornering rather than
  // which formula decided the number.
  TEST_ASSERT_TRUE_MESSAGE(slow < sqrtf(ACCEL * SMALL_R) * 1.3f,
    "the small circle should be held near the speed its own radius allows");
  TEST_ASSERT_TRUE_MESSAGE(fast < sqrtf(ACCEL * LARGE_R) * 1.3f,
    "and the large one near the speed its radius allows");
  TEST_ASSERT_TRUE_MESSAGE(slow > 1.0f && fast > 1.0f,
    "and neither should have stopped dead at every corner");
}

