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
 * The first tests of an LCD driver in this fork.
 *
 * A DWIN panel is not an interface the firmware calls; it is a screen the firmware writes
 * bytes at. So there is no equivalent of `stub_extui`'s recorded callbacks — the byte
 * stream on `LCD_SERIAL` is the only thing an outside observer can see, and a test that
 * cannot read it can only assert that the firmware did not crash while drawing.
 *
 * `SerialCapture` takes the port to watch, so the same drainer the host-facing tests use
 * works here. It has to drain from another thread: the write busy-waits for room in a
 * 128-byte buffer, and a screen refresh is longer than that, so draining afterwards would
 * be draining a buffer whose producer is already wedged.
 */

#include "../test/unit_tests.h"
#include "src/inc/MarlinConfig.h"

#if ENABLED(DWIN_CREALITY_LCD)

#include "src/lcd/dwin/creality/dwin.h"
#include "src/lcd/dwin/common/dwin_api.h"
#include "src/module/planner.h"
#include "src/lcd/marlinui.h"
#include "src/MarlinCore.h"
#include "../gcode/serial_capture.h"
#include "../support/simulated_encoder.h"
#include "../support/simulated_machine.h"
#include "../gcode/simulated_sensors.h"
#include "src/module/temperature.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/gcode/queue.h"
#include "src/module/motion.h"
#include "src/sd/cardreader.h"
#include <string.h>
#include <vector>
#include <utility>

namespace {

  /**
   * Walk a menu end to end, pressing at every row, and report where each press led.
   *
   * The pattern the main-menu test arrived at, factored out because every remaining HMI handler
   * in this driver is the same shape: a `switch` on the cursor with one arm per row. Three
   * things it deliberately does not assume, each of which was a bug in an earlier draft
   * somewhere in this file:
   *
   *   - **Which way the knob turns.** It is inverted between the main menu and the file list.
   *     Winding hard against the clamp reaches a known end whichever way it is wired.
   *   - **How far one turn moves the cursor.** Bounded by the clamp rather than by a count.
   *   - **That the knob is read at all between turns.** The menus go through
   *     `get_encoder_state()`, which ignores everything for `ENCODER_WAIT_MS` after each
   *     accepted event, so the pump has to let the clock move or every turn after the first is
   *     swallowed.
   *
   * `checkkey` is restored to `screen` before each press, so a row that navigates away does not
   * end the walk. Rows that do something instead of navigating — inject a command, save
   * settings — simply leave it unchanged and are reported as the screen itself.
   */
  template <typename Pump>
  std::vector<uint8_t> walk_a_menu(SimulatedEncoder &knob, Pump &&pump,
                                   const uint8_t screen, const uint8_t rows) {
    const uint8_t sweep = rows * 2 + 8;

    checkkey = screen;
    for (uint8_t i = 0; i < sweep; i++) knob.turn_counterclockwise(pump);

    std::vector<uint8_t> seen;
    for (uint8_t i = 0; i < sweep; i++) {
      checkkey = screen;
      knob.click(pump);
      if (seen.empty() || seen.back() != checkkey) seen.push_back(checkkey);
      checkkey = screen;
      knob.turn_clockwise(pump);
    }
    checkkey = screen;
    knob.click(pump);
    if (seen.empty() || seen.back() != checkkey) seen.push_back(checkkey);

    checkkey = ID_MainMenu;
    return seen;
  }

  /**
   * Assert that a walk visited exactly these screens, in the order the rows are drawn.
   *
   * The direction is taken from the walk rather than stated, for the reason the helper above
   * gives: which way the knob turns is a property of how this fixture and this panel happen to
   * agree, not a property of the firmware worth pinning. What *is* worth pinning is that the
   * rows lead to these screens, all of them different, adjacent in drawn order.
   *
   * The count is asserted before the order so a menu that gained or lost a row fails saying so,
   * rather than as a mismatch on whichever row happened to shift.
   */
  void the_walk_visited(const std::vector<uint8_t> &seen,
                        const std::vector<uint8_t> &forwards, const char * const menu) {
    char msg[200];
    snprintf(msg, sizeof(msg), "the %s menu should lead to %u distinct screens, saw %u",
             menu, unsigned(forwards.size()), unsigned(seen.size()));
    TEST_ASSERT_EQUAL_MESSAGE(forwards.size(), seen.size(), msg);

    const bool ascending = seen.front() == forwards.front();
    for (size_t k = 0; k < forwards.size(); k++) {
      const uint8_t expect = ascending ? forwards[k] : forwards[forwards.size() - 1 - k];
      snprintf(msg, sizeof(msg), "%s row %u should lead to screen %u, led to %u",
               menu, unsigned(k), unsigned(expect), unsigned(seen[k]));
      TEST_ASSERT_EQUAL_MESSAGE(expect, seen[k], msg);
    }
  }

}

MARLIN_TEST(dwin_display, a_status_message_reaches_the_panel) {
  SerialCapture panel(LCD_SERIAL);
  dwinStatusChanged("RESCUED");
  const std::string sent = panel.finish();

  TEST_ASSERT_TRUE_MESSAGE(sent.size() > 0,
    "changing the status should put bytes on the display's serial port");
}

#if ENABLED(EDITABLE_STEPS_PER_UNIT)

/**
 * Register #33, confirmed by behaviour rather than by reading the source.
 *
 * `planner.settings.axis_steps_per_mm` has a derived cache, `planner.mm_per_step`, which is
 * its reciprocal — and keeping the two in step is the caller's job, done by calling
 * `refresh_positioning()`. This driver writes the array and does not. So the machine goes on
 * moving at a scale that no longer matches the one it reports, and nothing complains.
 *
 * The test says exactly that and no more: after a person changes the resolution at the panel,
 * the stored value changed and the reciprocal did not follow it. It does not assert the
 * arithmetic of the new value — the driver scales by a factor private to its own translation
 * unit, and predicting it here would be re-implementing the code under test.
 *
 * Nothing is set behind the driver's back. The starting value is deliberately below the
 * driver's own lower limit, so the first turn of the knob is clamped up to it — which makes
 * the committed value the driver's choice rather than the test's.
 */
MARLIN_TEST(dwin_display, changing_the_resolution_at_the_panel_leaves_the_reciprocal_stale) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;              // a click on a dark panel only wakes the screen
  marlin.wait_for_user = false;     // ... and a click that resumes a wait is not an ENTER

  const float steps_before = planner.settings.axis_steps_per_mm[X_AXIS],
              per_step_before = planner.mm_per_step[X_AXIS];

  // The panel, showing the X steps-per-millimetre editor.
  checkkey = ID_StepValue;
  hmiFlag.step_axis = X_AXIS;
  hmiValues.maxStepScaled = 0;

  const auto pump = [] { dwinHandleScreen(); };
  knob.turn_clockwise(pump);
  knob.click(pump);

  panel.finish();

  const float steps_after = planner.settings.axis_steps_per_mm[X_AXIS];

  TEST_ASSERT_NOT_EQUAL_MESSAGE(steps_before, steps_after,
    "turning the knob and clicking should store a new resolution for X");

  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(per_step_before, planner.mm_per_step[X_AXIS],
    "the reciprocal should have gone stale - it is not refreshed after the write (register #33)");

  TEST_ASSERT_FALSE_MESSAGE(NEAR(planner.mm_per_step[X_AXIS], 1.0f / steps_after),
    "and being stale means it no longer is the reciprocal of the stored resolution");

  // Put the machine back the way it was found, both halves together this time.
  planner.set_steps_per_mm(X_AXIS, steps_before);
}

/**
 * The panel's other two value editors get it right, and that is why #33 is a defect.
 *
 * `hmiMaxFeedspeedXYZE()` and `hmiMaxAccelerationXYZE()` commit through
 * `planner.set_max_feedrate()` and `planner.set_max_acceleration()` — the setters that keep
 * whatever is derived from the value in step with it. `hmiStepXYZE()`, three functions away in
 * the same file, assigns `planner.settings.axis_steps_per_mm` directly and refreshes nothing.
 *
 * Pinning the two correct ones is not padding. It is what turns #33 from "this driver does not
 * bother with refreshes" into "this driver refreshes everywhere except one place", which is the
 * difference between a design decision and a bug — and it is what would notice if a fix for #33
 * broke its neighbours on the way past.
 *
 * The derived quantity for acceleration is `max_acceleration_steps_per_s2`, which the stepper
 * compares against on every block. Asserting it rather than the stored millimetre value is the
 * whole point: the stored value is right in both the working and the broken version.
 */
MARLIN_TEST(dwin_display, changing_the_acceleration_at_the_panel_refreshes_the_step_limit) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  const float accel_before = planner.settings.max_acceleration_mm_per_s2[X_AXIS];

  // The panel, showing the X maximum-acceleration editor, with a value below the driver's own
  // lower limit so the first turn is clamped up to it — the committed value is the driver's
  // choice rather than the test's, the same arrangement the steps editor test uses.
  checkkey = ID_MaxAccelerationValue;
  hmiFlag.acc_axis = X_AXIS;
  hmiValues.maxAcceleration = 0;

  const auto pump = [] { dwinHandleScreen(); };
  knob.turn_clockwise(pump);
  knob.click(pump);

  panel.finish();

  const float accel_after = planner.settings.max_acceleration_mm_per_s2[X_AXIS];

  TEST_ASSERT_NOT_EQUAL_MESSAGE(accel_before, accel_after,
    "turning the knob and clicking should store a new maximum acceleration for X");

  // The limit the stepper actually enforces is in steps, and it is derived from the value the
  // panel just wrote. `set_max_acceleration` refreshes it; a raw assignment would not.
  const uint32_t expected_steps = uint32_t(accel_after * planner.settings.axis_steps_per_mm[X_AXIS]);
  TEST_ASSERT_UINT32_WITHIN_MESSAGE(2, expected_steps, planner.max_acceleration_steps_per_s2[X_AXIS],
    "the step-rate limit should have been refreshed from the new acceleration");

  planner.set_max_acceleration(X_AXIS, accel_before);
}

/**
 * ...and the feedrate editor stores what the knob committed.
 *
 * Nothing is derived from a maximum feedrate in this build, so there is no stale-cache twin to
 * assert — which is worth saying rather than leaving as a gap in the pattern. What this pins is
 * the commit itself and the axis it lands on: `hmiFlag.feedspeed_axis` selects which of the
 * four values the click writes, and a panel that edited X and stored Y would be indetectable
 * from the screen.
 */
MARLIN_TEST(dwin_display, changing_the_feedrate_at_the_panel_stores_it_against_the_right_axis) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  const feedRate_t x_before = planner.settings.max_feedrate_mm_s[X_AXIS];
  #if HAS_Y_AXIS
    const feedRate_t y_before = planner.settings.max_feedrate_mm_s[Y_AXIS];
  #endif

  checkkey = ID_MaxSpeedValue;
  hmiFlag.feedspeed_axis = X_AXIS;
  hmiValues.maxFeedSpeed = 0;

  const auto pump = [] { dwinHandleScreen(); };
  knob.turn_clockwise(pump);
  knob.click(pump);

  panel.finish();

  TEST_ASSERT_NOT_EQUAL_MESSAGE(x_before, planner.settings.max_feedrate_mm_s[X_AXIS],
    "the click should have stored a new maximum feedrate for X");
  #if HAS_Y_AXIS
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(y_before, planner.settings.max_feedrate_mm_s[Y_AXIS],
      "and should not have touched another axis");
  #endif

  planner.set_max_feedrate(X_AXIS, x_before);
}

#endif // EDITABLE_STEPS_PER_UNIT

// ---------------------------------------------------------------------------
// Getting around the menus
// ---------------------------------------------------------------------------

/**
 * Every main-menu page leads somewhere, and to somewhere different — in adjacency order.
 *
 * This is the first thing anybody does with the panel: turn the knob until the icon they want
 * is highlighted, then press. The dispatch is a `switch` with one arm per page, so a machine
 * that sent every press to the same screen, or that had two arms transposed, would look
 * completely normal until you tried to use it. Pressing on one page only would leave three arms
 * unasserted — the same shape as `each_switch_is_reported_from_its_own_pin`: the deciding
 * element has to be moved through the collection rather than exercised once.
 *
 * **The test deliberately assumes neither the knob's direction nor its gearing.** A first
 * version hard-coded both and failed twice in different ways — clockwise turned out to
 * *decrease* the selection, and a `turn_clockwise()` call did not reliably move it one page.
 * Neither is a property of the firmware worth pinning here; both are properties of how this
 * fixture and this panel happen to agree, and encoding them would have made the test a
 * statement about the harness.
 *
 * **The menus also rate-limit the knob, where the value editors do not.** `hmiMainMenu()` reads
 * through `get_encoder_state()`, which ignores everything for `ENCODER_WAIT_MS` (20 ms) after
 * each accepted event; the value editors call `encoderReceiveAnalyze()` directly and have no
 * such gate. Under a HAL where time only moves when a test says so, that means consecutive
 * turns are simply swallowed — the first version of this test walked the whole menu and never
 * left the first page. Every step here lets the gate expire, which is also what a person's hand
 * does without thinking about it.
 *
 * So it winds hard to one end, then walks the other way one detent at a time, clicking at each
 * step and recording where it lands. What is asserted is the *sequence of distinct
 * destinations*, which is exactly the claim worth making: the four screens are reachable, they
 * are adjacent in the order the icons are drawn, and no two pages lead to the same place.
 *
 * `checkkey` is the observable rather than the internal selection, and it is the better one: it
 * is *where the press took you*, which is what the user experiences, and it is already part of
 * the driver's declared surface.
 */
MARLIN_TEST(dwin_display, the_main_menu_pages_lead_to_four_different_screens_in_order) {
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  // Every pump lets the encoder's 20 ms gate expire, or the turns after the first are ignored.
  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  // Wind hard to one end. `select_t::dec()` clamps at zero and `inc(4)` clamps at three, so
  // enough turns either way reaches a known end whichever way round the knob is wired.
  checkkey = ID_MainMenu;
  for (uint8_t i = 0; i < 20; i++) knob.turn_counterclockwise(pump);

  // Now walk back, clicking at every step, and collect the destinations in the order they
  // appear. Twenty steps is far more than four pages, so the walk is bounded by the clamp
  // rather than by a count that has to be right.
  std::vector<uint8_t> seen;
  for (uint8_t i = 0; i < 20; i++) {
    checkkey = ID_MainMenu;
    knob.click(pump);
    if (seen.empty() || seen.back() != checkkey) seen.push_back(checkkey);
    checkkey = ID_MainMenu;
    knob.turn_clockwise(pump);
  }

  // One last press at the far end, in case the final turn moved onto a page not yet pressed.
  checkkey = ID_MainMenu;
  knob.click(pump);
  if (seen.empty() || seen.back() != checkkey) seen.push_back(checkkey);

  panel.finish();
  checkkey = ID_MainMenu;

  const uint8_t last_page = TERN(HAS_ONESTEP_LEVELING, ID_Leveling, ID_Info);
  const uint8_t forwards[] = { ID_SelectFile, ID_Prepare, ID_Control, last_page };

  char msg[200];
  snprintf(msg, sizeof(msg), "walking the knob across the menu should visit four screens, saw %u",
           unsigned(seen.size()));
  TEST_ASSERT_EQUAL_MESSAGE(4, seen.size(), msg);

  // Either direction of travel is fine — which way the knob turns is the panel's business —
  // but the *order* must be the order the icons are drawn in.
  const bool ascending = seen.front() == forwards[0];
  for (size_t k = 0; k < 4; k++) {
    const uint8_t expect = ascending ? forwards[k] : forwards[3 - k];
    snprintf(msg, sizeof(msg), "screen %u along should be %u, was %u",
             unsigned(k), unsigned(expect), unsigned(seen[k]));
    TEST_ASSERT_EQUAL_MESSAGE(expect, seen[k], msg);
  }
}

// ---------------------------------------------------------------------------
// Stopping a print, which the panel asks about first
// ---------------------------------------------------------------------------

#if HAS_MEDIA

namespace {

  // Names must fit 8.3: this build has no long-filename support, and a longer stem is stored
  // mangled rather than refused.
  void put_file(const char * const name) {
    card.openFileWrite(name);
    card.write((void*)"G28\n", 4);
    card.closefile();
  }

  void send_gcode(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    const bool was = MYSERIAL1.host_connected;
    MYSERIAL1.host_connected = false;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    MYSERIAL1.host_connected = was;
  }

  // A machine on the print screen with a job under way, which is the state every button on
  // that screen is about. `abortFilePrintSoon()` only sets its flag when a file is open, so a
  // pretend print has to have really opened one.
  void a_print_is_running() {
    card.cdroot();
    put_file("RUNNING.GCO");
    card.cdroot();
    // `M23` then `M24`, the sequence a host uses. `openAndPrintFile()` looked like the direct
    // route and left no file open at all, which made two of the three tests below pass against
    // a machine that was not printing — see the precondition immediately after.
    send_gcode("M23 RUNNING.GCO");
    send_gcode("M24");
    card.flag.abort_sd_printing = false;
    // `abortFilePrintSoon()` sets its flag to `isFileOpen()`, so a fixture with no file open
    // makes "the print was not aborted" true for the wrong reason — and both the asks-first and
    // the declining test assert exactly that. Stating the precondition is what stops those two
    // passing vacuously.
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(),
      "the fixture should have a file open, or every assertion below is about nothing");
    checkkey = ID_PrintProcess;
    hmiFlag.done_confirm_flag = false;
    hmiFlag.pause_flag = false;
  }

  void stop_pretending_to_print() {
    card.abortFilePrintNow();
    card.flag.abort_sd_printing = false;
    card.cdroot();
    checkkey = ID_MainMenu;
  }

  /**
   * Put the print screen's cursor on Stop and press it.
   *
   * The print screen has three entries and Stop is the last, so one extreme of the knob is
   * Stop and the other is Setup. Which extreme is which is an agreement between this fixture
   * and the panel rather than a property of the firmware — it is inverted between the main menu
   * and the file list already — so this finds out by *pressing* and looking at where it landed,
   * rather than by reading the cursor. (The cursor is file-scope in `dwin.cpp` and exporting it
   * to satisfy a test would be the tail wagging the dog.)
   *
   * Landing on Setup opens the Tune menu, which is harmless and undone by putting the screen
   * back before the second attempt.
   */
  template <typename Pump> bool select_stop_on_the_print_screen(SimulatedEncoder &knob, Pump &&pump) {
    for (uint8_t attempt = 0; attempt < 2; attempt++) {
      checkkey = ID_PrintProcess;
      for (uint8_t i = 0; i < 8; i++)
        attempt ? knob.turn_clockwise(pump) : knob.turn_counterclockwise(pump);
      for (uint8_t i = 0; i < 8; i++)
        attempt ? knob.turn_counterclockwise(pump) : knob.turn_clockwise(pump);
      knob.click(pump);
      if (checkkey == ID_PrintWindow) return true;      // that end was Stop
    }
    return false;
  }

  /**
   * Answer the confirmation.
   *
   * `hmiFlag.select_flag` is the highlighted button and `drawSelectHighlight()` is the only
   * thing that sets it, so the answer is chosen by turning until the flag says what we want.
   * Turning rather than assigning is the point: assigning the flag would test the branch
   * without testing that the knob can reach it.
   */
  template <typename Pump> void answer_the_popup(SimulatedEncoder &knob, Pump &&pump, const bool yes) {
    for (uint8_t i = 0; i < 6 && hmiFlag.select_flag != yes; i++) {
      knob.turn_clockwise(pump);
      if (hmiFlag.select_flag != yes) knob.turn_counterclockwise(pump);
    }
    knob.click(pump);
  }

}


/**
 * Pressing Stop asks before it stops.
 *
 * A print is hours of work and a knob is easy to knock, so `hmiPrinting()` does not act on the
 * press: it sets `checkkey = ID_PrintWindow` and draws a confirmation. Everything about that is
 * worth pinning, because a driver that stopped immediately would look identical on the bench —
 * you only find out the difference on a print you cared about.
 *
 * The card is asserted still printing afterwards, not merely the screen id: the screen changing
 * is what the *user* sees, and the print continuing is what actually matters.
 */
MARLIN_TEST(dwin_display, pressing_stop_asks_before_it_stops) {
  SimulatedMachine machine;
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  a_print_is_running();

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  bool found_stop = false;
  { SerialCapture panel(LCD_SERIAL); found_stop = select_stop_on_the_print_screen(knob, pump); panel.finish(); }
  TEST_ASSERT_TRUE_MESSAGE(found_stop, "one end of the print screen should be Stop");

  TEST_ASSERT_EQUAL_MESSAGE(ID_PrintWindow, checkkey,
    "pressing Stop should open a confirmation, not stop the print");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.abort_sd_printing,
    "and the print should still be running while the question is on screen");

  stop_pretending_to_print();
}

/**
 * Saying yes stops it.
 *
 * `hmiFlag.select_flag` is which button the popup has highlighted, and `drawSelectHighlight()`
 * is what sets it — so the two arms of the confirmation are reached by turning the knob before
 * pressing, exactly as a person does. Confirming asks the card to abort, which the main loop
 * then acts on.
 */
MARLIN_TEST(dwin_display, confirming_the_stop_aborts_the_print) {
  SimulatedMachine machine;
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  a_print_is_running();

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  {
    SerialCapture panel(LCD_SERIAL);
    TEST_ASSERT_TRUE(select_stop_on_the_print_screen(knob, pump));
    answer_the_popup(knob, pump, true);
    panel.finish();
  }

  TEST_ASSERT_TRUE_MESSAGE(card.flag.abort_sd_printing,
    "confirming should ask the card to abort the print");

  stop_pretending_to_print();
}

/**
 * Saying no leaves it running — the arm that makes the question worth asking.
 *
 * A confirmation that stops the print whichever button you choose is worse than none at all: it
 * looks like a safeguard and is a second way to lose the job. The declining arm calls
 * `gotoPrintProcess()` and touches nothing else, so what is asserted is that the print survived
 * *and* that the screen went back to where it was.
 */
MARLIN_TEST(dwin_display, declining_the_stop_leaves_the_print_running) {
  SimulatedMachine machine;
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  a_print_is_running();

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  {
    SerialCapture panel(LCD_SERIAL);
    TEST_ASSERT_TRUE(select_stop_on_the_print_screen(knob, pump));
    answer_the_popup(knob, pump, false);
    panel.finish();
  }

  TEST_ASSERT_FALSE_MESSAGE(card.flag.abort_sd_printing,
    "declining should leave the print running");
  TEST_ASSERT_EQUAL_MESSAGE(ID_PrintProcess, checkkey,
    "and should put the print screen back");

  stop_pretending_to_print();
}

#endif // HAS_MEDIA

// ---------------------------------------------------------------------------
// Keeping the panel's numbers agreeing with the machine
// ---------------------------------------------------------------------------

/**
 * The panel is redrawn only where something changed.
 *
 * `updateVariable()` runs on every UI pass and holds a `static` cache of each displayed value —
 * the two temperatures, their targets, the fan, the flow, the feedrate, the babystep offset.
 * Every one is compared and only redrawn on a difference. That is not an optimisation to be
 * traded away: the link to the panel is a 128-byte buffer that the firmware busy-waits on, so a
 * screen redrawn wholesale on every pass would spend the print blocking on it. It is the same
 * property `SerialCapture` needs a second thread for.
 *
 * The assertion is on *how much* came out rather than on what it said. Decoding the DWIN wire
 * format would pin the protocol — pixel positions, font ids, the lot — and none of that is the
 * behaviour under test. Traffic volume is: a pass with nothing to say must cost less than a
 * pass with something to say.
 *
 * The caches are process-wide statics, so the first call after anything else has touched the
 * machine reports everything. Priming is therefore part of the arrangement rather than
 * defensive padding.
 */
MARLIN_TEST(dwin_display, the_panel_is_redrawn_only_where_something_changed) {
  SimulatedMachine machine;
  SimulatedSensors sensors;

  checkkey = ID_MainMenu;

  // Prime: whatever the caches were holding, get them agreeing with the machine now.
  { SerialCapture panel(LCD_SERIAL); updateVariable(); panel.finish(); }

  size_t quiet = 0, quiet_again = 0, after_change = 0;

  { SerialCapture panel(LCD_SERIAL); updateVariable(); quiet = panel.finish().size(); }
  { SerialCapture panel(LCD_SERIAL); updateVariable(); quiet_again = panel.finish().size(); }

  // Not merely "the same" — *nothing*. Measured rather than assumed: a settled machine costs
  // 0 bytes and a changed hotend target costs 42, so this is the strong form of the claim
  // rather than a bound that a wasteful implementation could also satisfy.
  TEST_ASSERT_EQUAL_MESSAGE(0, quiet,
    "a pass over a machine that has not changed should send the panel nothing at all");
  TEST_ASSERT_EQUAL_MESSAGE(0, quiet_again,
    "and should keep sending nothing: the cache should settle, not oscillate");

  const celsius_t was = thermalManager.degTargetHotend(0);
  thermalManager.setTargetHotend(was + 40, 0);

  { SerialCapture panel(LCD_SERIAL); updateVariable(); after_change = panel.finish().size(); }

  char msg[160];
  snprintf(msg, sizeof(msg),
           "a changed target should reach the panel: quiet pass was %u bytes, changed pass %u",
           unsigned(quiet), unsigned(after_change));
  TEST_ASSERT_TRUE_MESSAGE(after_change > quiet, msg);

  thermalManager.setTargetHotend(was, 0);
  { SerialCapture panel(LCD_SERIAL); updateVariable(); panel.finish(); }
}

// ---------------------------------------------------------------------------
// Choosing a file
// ---------------------------------------------------------------------------

#if HAS_MEDIA


/**
 * Inside a folder, one row means "up", and at the root no row does.
 *
 * The list has a variable prologue: row 0 is always Back, row 1 is `..` *only* when the machine
 * is inside a folder, and the file under row `n` is therefore `n - 1 - hasUpDir`. That trailing
 * term is the whole of this test's subject. When it is wrong the panel highlights one name and
 * opens its neighbour — which on a printer means starting the wrong job, and a print is hours.
 *
 * The mutation run asked for this: twenty-eight survivors sat on the two lines that compute it,
 * because every existing test of this screen runs at the card root, where `hasUpDir` is zero and
 * the term disappears. **A test that never leaves the default state cannot see a correction that
 * only applies outside it.**
 *
 * Two arms, and the second is the one that pins the arithmetic rather than the row. The first
 * says the row after Back leaves the folder; the second says the *far* end still opens the
 * folder's last entry, which is the same claim the root test makes one offset along. An
 * implementation with `hasUpDir` stuck at 1, or dropped altogether, gets the near row right and
 * every file row wrong by one — so the near row alone is not enough.
 *
 * The root arm this test first had was dropped rather than fixed, and why is worth keeping: it
 * asserted that pressing the row after Back at the root does not climb above it. That is
 * satisfied by every implementation, because `cdup()` at the root is already a no-op — so the
 * mutant it was aimed at survives it. It also failed against correct firmware, because the row
 * after Back at the root is simply the card's first entry, and this test had just put a folder
 * there. The root case is covered by `both_ends_of_the_file_list_lead_where_they_should`.
 *
 * The rows are reached by winding hard onto the Back clamp and stepping off it, so nothing here
 * assumes which way the knob turns; which clamp is Back is asked rather than assumed, and
 * pressing Back to find out is free because it only returns to the main menu.
 */
MARLIN_TEST(dwin_display, inside_a_folder_one_row_goes_up_and_the_files_sit_below_it) {
  SimulatedMachine machine;
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };
  constexpr uint8_t ROOM = 40;

  card.cdroot();
  {
    MediaFile root = card.getroot(), made;
    made.mkdir(&root, "PANELDIR");
    made.close();
  }
  card.cd("PANELDIR");
  put_file("AINNER.GCO");
  put_file("ZINNER.GCO");
  card.cdroot();

  // Which clamp is Back? Free to ask — pressing Back only returns to the main menu.
  checkkey = ID_SelectFile;
  for (uint8_t i = 0; i < ROOM; i++) { knob.turn_clockwise(pump); checkkey = ID_SelectFile; }
  knob.click(pump);
  const bool back_is_clockwise = (checkkey == ID_MainMenu);

  const auto go_to_the_row_after_back = [&] {
    checkkey = ID_SelectFile;
    for (uint8_t i = 0; i < ROOM; i++) {
      if (back_is_clockwise) knob.turn_clockwise(pump); else knob.turn_counterclockwise(pump);
      checkkey = ID_SelectFile;
    }
    if (back_is_clockwise) knob.turn_counterclockwise(pump); else knob.turn_clockwise(pump);
    checkkey = ID_SelectFile;
  };

  // Inside the folder, that row goes up.
  card.cd("PANELDIR");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.workDirIsRoot,
    "this half of the test is about a machine inside a folder");

  go_to_the_row_after_back();
  knob.click(pump);

  TEST_ASSERT_TRUE_MESSAGE(card.flag.workDirIsRoot,
    "the row after Back should be '..' inside a folder, and should leave the folder");
  TEST_ASSERT_FALSE_MESSAGE(card.isFileOpen(),
    "and should not have opened anything on the way out");

  // ...and the far end still lands on the folder's last entry, one row further down than it
  // would be at the root. This is the half that separates a wrong `hasUpDir` from a right one.
  card.cd("PANELDIR");
  const uint16_t inner = card.get_num_items();
  TEST_ASSERT_TRUE_MESSAGE(inner >= 2,
    "the folder needs at least two entries, or 'the last one' cannot be told from 'the first'");

  // Read the expectation before acting: `selectFileByIndexSorted` is what the driver uses too,
  // and calling it afterwards would overwrite the evidence with the answer.
  card.selectFileByIndexSorted(0);
  const std::string first_in_folder(card.filename);
  card.selectFileByIndexSorted(1);
  const std::string second_in_folder(card.filename);
  TEST_ASSERT_TRUE_MESSAGE(first_in_folder != second_in_folder,
    "the two entries must be distinguishable or an off-by-one cannot be seen");

  // Two rows below Back — under "Back" and under ".." — is the folder's *first* entry.
  //
  // The first version of this arm pressed the far clamp instead, and an implementation that
  // drops `hasUpDir` from the index passed it: at the clamp the wrong index is one *past* the
  // end, and `selectFileByIndexSorted` leaves `card.filename` alone rather than reporting a
  // different file. An off-by-one is only visible on a row where both answers are in range.
  go_to_the_row_after_back();
  if (back_is_clockwise) knob.turn_counterclockwise(pump); else knob.turn_clockwise(pump);
  checkkey = ID_SelectFile;
  knob.click(pump);

  TEST_ASSERT_EQUAL_STRING_MESSAGE(first_in_folder.c_str(), card.filename,
    "two rows below Back should be the folder's first entry: Back, then '..', then the files");

  card.abortFilePrintNow();
  card.cdroot();
  checkkey = ID_MainMenu;
}

/**
 * Both ends of the file list, and neither is assumed.
 *
 * The list is one row of "Back" followed by the card's items, so the file under row `n` is
 * `n - 1` — and `n - 1 - hasUpDir` once you are inside a folder. Every index is offset by
 * something that is not always the same, which is exactly the arithmetic that goes wrong
 * quietly: the panel highlights the name you wanted and opens the one above or below it.
 *
 * So the test leans on the knob to each extreme in turn and presses. One end must be "Back" —
 * the only way off this screen, and a panel you cannot leave is a panel you power-cycle. The
 * other must be the card's *last* entry: `select_file.inc(1 + fullCnt)` clamps there, and if
 * the bound were one out the press would either open the wrong file or index past the end of
 * the list.
 *
 * **Nothing here assumes which way the knob turns, or how many files are on the card**, and
 * both of those were mistakes in earlier drafts. The direction is a property of how the fixture
 * and the panel happen to agree — it turned out to be the opposite of the main menu's — and the
 * card is shared: other tests leave files on it, so "the last file" is whatever the card says
 * it is, not whatever this test wrote. Winding hard against a clamp and asking the card for its
 * own count removes both.
 */
MARLIN_TEST(dwin_display, both_ends_of_the_file_list_lead_where_they_should) {
  SimulatedMachine machine;
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  card.cdroot();
  put_file("AAA.GCO");
  put_file("BBB.GCO");
  put_file("CCC.GCO");
  card.cdroot();                     // re-read the directory now the files are there

  const uint16_t items = card.get_num_items();
  TEST_ASSERT_TRUE_MESSAGE(items >= 3, "the fixture should have put files on the card");

  // What the far end *should* reach: the last entry in the card's own sorted order. Read now,
  // because `selectFileByIndexSorted` is also what the driver uses and it overwrites
  // `card.filename` — the first draft of this test computed the expectation afterwards and so
  // compared the value against itself.
  card.selectFileByIndexSorted(items - 1);
  const std::string last_on_card(card.filename);
  card.selectFileByIndexSorted(0);
  const std::string first_on_card(card.filename);
  TEST_ASSERT_TRUE_MESSAGE(first_on_card != last_on_card,
    "this test needs the first and last entries to differ, or it cannot see an off-by-one");

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  // Press at each extreme. Which extreme is which is the panel's business, not the test's.
  struct End { uint8_t landed_on; std::string reached_for; };
  End ends[2];

  for (uint8_t which = 0; which < 2; which++) {
    card.cdroot();
    checkkey = ID_SelectFile;
    SerialCapture panel(LCD_SERIAL);

    // Wind hard the other way first, so each pass starts from a known end rather than from
    // wherever the previous press left the cursor. The number of turns is derived from the
    // card rather than written down: this card is shared with every other test that writes a
    // file, so a fixed count that crossed the list today would stop short tomorrow — which is
    // how an earlier draft came to press on a file in the middle and report it as the last.
    const uint16_t sweep = items * 2 + 8;
    for (uint16_t i = 0; i < sweep; i++)
      which ? knob.turn_clockwise(pump) : knob.turn_counterclockwise(pump);
    for (uint16_t i = 0; i < sweep; i++)
      which ? knob.turn_counterclockwise(pump) : knob.turn_clockwise(pump);

    knob.click(pump);
    panel.finish();

    ends[which] = { checkkey, std::string(card.filename) };
    card.abortFilePrintNow();        // stop anything the press started
    checkkey = ID_MainMenu;
  }

  card.cdroot();

  const bool first_is_back = ends[0].landed_on == ID_MainMenu;
  const End &back = first_is_back ? ends[0] : ends[1];
  const End &file = first_is_back ? ends[1] : ends[0];

  TEST_ASSERT_EQUAL_MESSAGE(ID_MainMenu, back.landed_on,
    "one end of the file list must be Back, which is the only way off this screen");
  TEST_ASSERT_NOT_EQUAL_MESSAGE(ID_MainMenu, file.landed_on,
    "and the other end must be a file, not a second way out");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(last_on_card.c_str(), file.reached_for.c_str(),
    "the far end should stop on the card's last entry, not one past it or one short");
}

#endif // HAS_MEDIA

// ---------------------------------------------------------------------------
// The Control menu
// ---------------------------------------------------------------------------

/**
 * Every row of the Control menu leads to its own screen.
 *
 * The same claim as the main menu, one level down, and it is worth making again rather than
 * assuming: this is a second hand-written `switch` with its own row constants, and the way these
 * go wrong is two arms transposed — you press Motion and get the temperature editor. Nothing
 * about the code looks different when that happens.
 *
 * Under `010-dwin` the destructive rows are not compiled: `EEPROM_SETTINGS` is off, so Save,
 * Load and Reset are absent and the walk cannot fire `settings.reset()` part-way through the
 * suite. That is checked rather than assumed — with EEPROM on, this test would need to skip
 * those rows rather than press them.
 */
MARLIN_TEST(dwin_display, every_control_menu_row_leads_to_its_own_screen) {
  SimulatedMachine machine;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  const std::vector<uint8_t> seen = walk_a_menu(knob, pump, ID_Control, 6);
  panel.finish();

  // Back, Temperature, Motion, Advanced Settings, Info — in the order the rows are drawn.
  the_walk_visited(seen, { ID_MainMenu, ID_TemperatureID, ID_Motion, ID_AdvSet, ID_Info },
                   "Control");
}

// ---------------------------------------------------------------------------
// The Motion menu
// ---------------------------------------------------------------------------

/**
 * Every row of the Motion menu opens the editor for its own limit.
 *
 * This is the menu that matters most to the `planner.settings` migration: its four rows are the
 * only way a person standing at the machine can change the feedrate ceiling, the acceleration
 * ceiling and the steps-per-millimetre — and two of those three commit through the setters that
 * keep the derived limits in step while the third does not (register #33). Before any of that
 * can be asserted, the row a person presses has to be the editor they get.
 *
 * The failure this catches is two arms transposed. Feedrate and acceleration are adjacent rows
 * holding adjacent-looking numbers, and a machine that opened the acceleration editor when you
 * asked for feedrate would let somebody set 3000 mm/s believing they had set 3000 mm/s². The
 * value they typed would be accepted, stored, and wrong, and nothing would say so.
 *
 * The expected list is built from the same `ENABLED(CLASSIC_JERK)` the row constants use, so a
 * build with jerk enabled asserts five rows rather than failing on four.
 */
MARLIN_TEST(dwin_display, every_motion_menu_row_opens_its_own_limit) {
  SimulatedMachine machine;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  std::vector<uint8_t> forwards = { ID_Control, ID_MaxSpeed, ID_MaxAcceleration };
  TERN_(CLASSIC_JERK, forwards.push_back(ID_MaxJerk));
  forwards.push_back(ID_Step);

  const std::vector<uint8_t> seen = walk_a_menu(knob, pump, ID_Motion, forwards.size());
  panel.finish();

  the_walk_visited(seen, forwards, "Motion");
}

// ---------------------------------------------------------------------------
// The Temperature menu
// ---------------------------------------------------------------------------

/**
 * Every row of the Temperature menu opens the editor for its own heater.
 *
 * The same claim again, and the consequence of getting it wrong is the most direct in the
 * driver: the hotend and the bed sit next to each other, both edited as a bare three-digit
 * number, and their safe ranges do not overlap. A machine that opened the hotend editor when
 * the operator pressed Bed would take 220 without complaint and drive the nozzle to a
 * temperature they never asked for.
 *
 * The last two rows are the preheat *settings* screens rather than actions — pressing them
 * changes no target, which is what makes this menu safe to walk in the middle of a suite.
 * Checked rather than assumed: the arms set `checkkey` and draw, and touch no heater.
 */
MARLIN_TEST(dwin_display, every_temperature_menu_row_opens_its_own_heater) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  std::vector<uint8_t> forwards = { ID_Control };
  TERN_(HAS_HOTEND, forwards.push_back(ID_ETemp));
  TERN_(HAS_HEATED_BED, forwards.push_back(ID_BedTemp));
  TERN_(HAS_FAN, forwards.push_back(ID_FanSpeed));
  #if HAS_PREHEAT
    forwards.push_back(ID_PLAPreheat);
    #if PREHEAT_COUNT > 1
      forwards.push_back(ID_ABSPreheat);
    #endif
  #endif

  const std::vector<uint8_t> seen =
    walk_a_menu(knob, pump, ID_TemperatureID, forwards.size());
  panel.finish();

  the_walk_visited(seen, forwards, "Temperature");
}

// ---------------------------------------------------------------------------
// The Move menu, and the one row it will not open
// ---------------------------------------------------------------------------

namespace {

  #if ENABLED(PREVENT_COLD_EXTRUSION)
    /**
     * Restores whether the machine will extrude cold, and clears the popup's latch.
     *
     * Both are machine state that outlives a test. `allow_cold_extrude` is the flag `M302`
     * sets, and the E-row test below turns it off deliberately; `hmiFlag.cold_flag` is the
     * driver's own latch, which swallows every encoder event until it is answered — so a test
     * that left it set would hand the next one a panel that ignores the knob.
     */
    struct ColdExtrusionRule {
      bool was;
      ColdExtrusionRule() : was(thermalManager.allow_cold_extrude) {}
      ~ColdExtrusionRule() {
        thermalManager.allow_cold_extrude = was;
        hmiFlag.cold_flag = false;
      }
    };
  #endif

}

/**
 * Every row of the Move menu opens the mover for its own axis.
 *
 * Transposing two arms here moves the wrong axis, and the move menu is where a person nudges a
 * nozzle that is already close to something — the bed, a clip, a finished part. Asking for X
 * and getting Z is the one mistake in this driver that can drive the nozzle into the bed while
 * the operator is watching the number they asked for go up.
 *
 * The extruder row is opened here with cold extrusion permitted, so that this test is about the
 * dispatch and the next one is about the guard. Without that, the E row would trip the cold
 * warning and the walk would report a fifth screen that is really the fourth.
 */
MARLIN_TEST(dwin_display, every_move_menu_row_opens_its_own_axis) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  #if ENABLED(PREVENT_COLD_EXTRUSION)
    ColdExtrusionRule cold_rule;
    thermalManager.allow_cold_extrude = true;
  #endif

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  std::vector<uint8_t> forwards = { ID_Prepare, ID_MoveX, ID_MoveY, ID_MoveZ };
  TERN_(HAS_HOTEND, forwards.push_back(ID_Extruder));

  const std::vector<uint8_t> seen = walk_a_menu(knob, pump, ID_AxisMove, forwards.size());
  panel.finish();

  the_walk_visited(seen, forwards, "Move");
}

#if ALL(PREVENT_COLD_EXTRUSION, HAS_HOTEND)

/**
 * The extruder will not be moved through a cold nozzle, and the panel says why.
 *
 * Filament that is not molten does not go through a 0.4 mm hole. What happens instead is that
 * the drive gear chews a flat into the filament until it can no longer grip anything at all,
 * and the machine then cannot print until somebody dismantles the extruder. It is the most
 * common way to break a printer from the front panel, which is why the guard exists.
 *
 * Two things are asserted, and one alone would not do:
 *
 *   - the editor **did not open**, so the knob cannot be turned into a move; and
 *   - the panel was told *why*, in words. A refusal with no explanation reads as a dead button,
 *     and the next thing a person does with a dead button is press it harder.
 *
 * The warning is asserted as the characters `Nozzle is too cold`, which is content the driver
 * was told to display, rather than as a count of bytes — the coordinates and font ids around it
 * are protocol this test has no business pinning.
 *
 * Which end of the list the extruder sits at is found rather than assumed, by the same argument
 * as everywhere else in this file: the knob's direction is inverted between screens here. It is
 * found with the guard *lifted*, so that probing for the row cannot trip the behaviour under
 * test before the test has started.
 */
MARLIN_TEST(dwin_display, a_cold_nozzle_will_not_open_the_extruder_mover) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;

  ColdExtrusionRule cold_rule;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };
  constexpr uint8_t SWEEP = 20;

  // Which extreme is the extruder? Ask with the guard lifted, so the asking cannot trip it.
  thermalManager.allow_cold_extrude = true;
  bool extruder_is_clockwise = false;
  {
    SerialCapture quiet(LCD_SERIAL);
    checkkey = ID_AxisMove;
    for (uint8_t i = 0; i < SWEEP; i++) knob.turn_clockwise(pump);
    checkkey = ID_AxisMove;
    knob.click(pump);
    extruder_is_clockwise = (checkkey == ID_Extruder);
    quiet.finish();
  }
  // Either end is legitimate, but it must be *an* end: a probe that landed in the middle of the
  // list would send the rest of this test at whichever row it happened to stop on.
  TEST_ASSERT_TRUE_MESSAGE(extruder_is_clockwise || checkkey == ID_Prepare,
    "winding hard should reach a clamp — either the extruder row or Back");

  // Now the machine a person actually has: a nozzle at room temperature and no override.
  thermalManager.allow_cold_extrude = false;
  hmiFlag.cold_flag = false;
  SimulatedSensors::hotend_reads(20.0f);
  for (uint16_t i = 0; i < 400; i++) { HAL_test_advance_millis(1); thermalManager.task(); }
  TEST_ASSERT_TRUE_MESSAGE(thermalManager.tooColdToExtrude(0),
    "this test is about a nozzle the firmware considers too cold to extrude through");

  std::string drawn;
  {
    SerialCapture panel(LCD_SERIAL);
    checkkey = ID_AxisMove;
    for (uint8_t i = 0; i < SWEEP; i++) {
      if (extruder_is_clockwise) knob.turn_clockwise(pump); else knob.turn_counterclockwise(pump);
      checkkey = ID_AxisMove;
    }
    knob.click(pump);
    drawn = panel.finish();
  }

  TEST_ASSERT_EQUAL_MESSAGE(ID_AxisMove, checkkey,
    "a cold nozzle should leave the panel on the move menu, not in the extruder editor");
  TEST_ASSERT_TRUE_MESSAGE(drawn.find("Nozzle is too cold") != std::string::npos,
    "and should say why, rather than refusing silently");

  checkkey = ID_MainMenu;
}

#endif // PREVENT_COLD_EXTRUSION && HAS_HOTEND

// ---------------------------------------------------------------------------
// The Prepare menu, which is a menu of actions rather than of screens
// ---------------------------------------------------------------------------

/**
 * `walk_a_menu()` is the wrong instrument for this one, and knowing why is the point.
 *
 * It answers "where did each row lead", and most of the Prepare menu's rows do not lead
 * anywhere: they release the motors, start a homing move, preheat for a material, cool
 * everything down, change the language. `checkkey` is untouched by five of them, so a walk would
 * collapse those five into one entry and then compare a short list against a short expectation —
 * a green test making a much weaker claim than it appears to.
 *
 * What separates these rows is their *effect*, so that is what gets recorded. Each press is
 * preceded by putting the heaters at a marker value nothing else in the menu produces, which
 * makes "this row changed no temperature" a positive observation rather than an absence.
 */
namespace {

  struct RowEffect {
    uint8_t landed_on;
    celsius_t hotend, bed;
    uint8_t language;
  };

  #if HAS_HOTEND && HAS_HEATED_BED

    // Values no row in this menu can produce: not zero (cooldown), and not either preset.
    constexpr celsius_t MARKER_HOTEND = 111, MARKER_BED = 77;

    /**
     * Restores everything the walk below disturbs.
     *
     * The language is panel state that outlives the test and has no other way back — the row
     * *toggles*, so a walk that pressed it an odd number of times would leave every later test
     * reading a Chinese menu. The heater targets are the usual `longjmp` hazard.
     */
    struct PrepareMenuState {
      uint8_t was_language;
      celsius_t was_hotend, was_bed;
      PrepareMenuState()
        : was_language(hmiFlag.language),
          was_hotend(thermalManager.degTargetHotend(0)),
          was_bed(thermalManager.degTargetBed()) {}
      ~PrepareMenuState() {
        hmiFlag.language = was_language;
        thermalManager.setTargetHotend(was_hotend, 0);
        thermalManager.setTargetBed(was_bed);
        queue.clear();
        checkkey = ID_MainMenu;
      }
    };

    /**
     * Press every row of the Prepare menu once, and report what each press did.
     *
     * The walk runs from the far clamp *down to* Back rather than up from it, and stops the
     * moment a press lands on the main menu. That is what discovers the row count instead of
     * stating it — the row constants are `#define`s inside the driver and invisible here — and
     * it is also what stops the walk pressing a clamped row twice, which for the language row
     * would mean toggling it an unpredictable number of times.
     *
     * Back is the anchor because it is the only row whose outcome is unambiguous from outside.
     */
    template <typename Pump>
    std::vector<RowEffect> walk_the_prepare_menu(SimulatedEncoder &knob, Pump &&pump) {
      constexpr uint8_t ROOM = 24;

      // Which clamp is Back? Asked rather than assumed, and the asking is free because pressing
      // Back only returns to the main menu. The fixture's idea of "clockwise" is a phase
      // sequence; which direction the firmware derives from it is the firmware's business, and
      // the first draft of this walk got it backwards and reported a menu one row long.
      checkkey = ID_Prepare;
      for (uint8_t i = 0; i < ROOM; i++) { knob.turn_clockwise(pump); checkkey = ID_Prepare; }
      knob.click(pump);
      const bool back_is_clockwise = (checkkey == ID_MainMenu);

      const auto to_the_far_end = [&] {
        checkkey = ID_Prepare;
        for (uint8_t i = 0; i < ROOM; i++) {
          if (back_is_clockwise) knob.turn_counterclockwise(pump); else knob.turn_clockwise(pump);
          checkkey = ID_Prepare;
        }
      };
      const auto towards_back = [&] {
        if (back_is_clockwise) knob.turn_clockwise(pump); else knob.turn_counterclockwise(pump);
      };

      to_the_far_end();

      std::vector<RowEffect> backwards;
      for (uint8_t i = 0; i < ROOM; i++) {
        thermalManager.setTargetHotend(MARKER_HOTEND, 0);
        thermalManager.setTargetBed(MARKER_BED);
        checkkey = ID_Prepare;

        knob.click(pump);

        backwards.push_back({ checkkey, thermalManager.degTargetHotend(0),
                              thermalManager.degTargetBed(), hmiFlag.language });
        if (checkkey == ID_MainMenu) break;

        checkkey = ID_Prepare;
        towards_back();
      }

      std::vector<RowEffect> rows(backwards.rbegin(), backwards.rend());
      return rows;
    }

  #endif // HAS_HOTEND && HAS_HEATED_BED

}

#if HAS_HOTEND && HAS_HEATED_BED && HAS_PREHEAT

/**
 * Preheating names a material, and the row that names it is the one that heats for it.
 *
 * Every preset is two numbers that have to arrive together: a nozzle temperature and a bed
 * temperature chosen for the same plastic. Transposing the two rows gives a machine that heats
 * the bed for ABS and the nozzle for PLA, and the result is a print that will not stick — or, the
 * other way round, a nozzle held 60 degrees above what the filament in it can take, cooking it
 * into a blockage while the operator waits for the beep.
 *
 * Asserting one preset alone would pass against a driver that always preheated with the first,
 * which is why both rows are found and both pairs are checked. The presets are asserted to differ
 * first: with two identical presets this test would be satisfied by any wiring at all, and that
 * is a property of the configuration rather than of the code.
 *
 * The rows are located by effect rather than by index. `PREPARE_CASE_PLA` is a `#define` inside
 * the driver, computed from four `ENABLED()` terms, so a test naming a number would be asserting
 * against an arithmetic it cannot see and would drift silently the day a feature is turned on.
 */
MARLIN_TEST(dwin_display, preheating_from_the_prepare_menu_uses_the_material_it_names) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;
  PrepareMenuState saved;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };
  const std::vector<RowEffect> rows = walk_the_prepare_menu(knob, pump);
  panel.finish();

  TEST_ASSERT_TRUE_MESSAGE(rows.size() >= 2,
    "the walk should have anchored on Back and reported every row above it");

  #if PREHEAT_COUNT > 1
    const bool presets_differ =
      ui.material_preset[0].hotend_temp != ui.material_preset[1].hotend_temp ||
      ui.material_preset[0].bed_temp    != ui.material_preset[1].bed_temp;
    TEST_ASSERT_TRUE_MESSAGE(presets_differ,
      "this test can only distinguish the two rows if the two presets differ");
  #endif

  int found_first = -1, found_second = -1;
  for (size_t r = 0; r < rows.size(); r++) {
    if (rows[r].hotend == ui.material_preset[0].hotend_temp &&
        rows[r].bed    == ui.material_preset[0].bed_temp) found_first = int(r);
    #if PREHEAT_COUNT > 1
      if (rows[r].hotend == ui.material_preset[1].hotend_temp &&
          rows[r].bed    == ui.material_preset[1].bed_temp) found_second = int(r);
    #endif
  }

  TEST_ASSERT_TRUE_MESSAGE(found_first >= 0,
    "one row should heat both the nozzle and the bed for the first material");
  #if PREHEAT_COUNT > 1
    TEST_ASSERT_TRUE_MESSAGE(found_second >= 0,
      "and another should do the same for the second");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(found_first, found_second,
      "and they should be two different rows, not one row doing both");
    TEST_ASSERT_TRUE_MESSAGE(found_first < found_second,
      "with the materials in the order the rows are drawn");
  #endif
}

/**
 * One row turns everything off, and everything means the bed as well.
 *
 * The bed is the heater people forget: it is out of sight under the print, it holds 60 degrees
 * without any of the noise or smell that says a nozzle is hot, and a machine left with the bed on
 * overnight is a machine drawing a couple of hundred watts into an empty room. A cooldown row
 * that zeroed only the hotend would look right from the front panel — the number a person watches
 * is the nozzle's — and would be wrong in exactly the way nobody checks.
 *
 * The marker values are what make this a claim about cooldown rather than about a cold machine:
 * both heaters are given a non-zero target immediately before every press, so the only row that
 * can report zero is one that actively cleared them.
 */
MARLIN_TEST(dwin_display, one_prepare_row_turns_every_heater_off) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;
  PrepareMenuState saved;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };
  const std::vector<RowEffect> rows = walk_the_prepare_menu(knob, pump);
  panel.finish();

  int cooled = 0, half_cooled = 0;
  for (const RowEffect &row : rows) {
    if (row.hotend == 0 && row.bed == 0) cooled++;
    else if (row.hotend == 0 || row.bed == 0) half_cooled++;
  }

  TEST_ASSERT_EQUAL_MESSAGE(1, cooled,
    "exactly one row should turn both heaters off");
  TEST_ASSERT_EQUAL_MESSAGE(0, half_cooled,
    "and no row should turn off one heater while leaving the other running");
}

/**
 * The language row swaps the two, rather than setting one of them.
 *
 * It is the only way to change the panel's language, so it has to work in both directions: a
 * driver that assigned rather than toggled would leave anyone who pressed it once unable to get
 * back, reading a menu they cannot follow with no other control that would help them. That is
 * a machine returned as broken.
 *
 * Asserting the first press alone cannot see the difference — assignment and toggle agree on the
 * way out — so the row is pressed twice and the language is asserted to have moved and returned.
 * The two presses are separate walks, because a walk deliberately presses each row only once.
 */
MARLIN_TEST(dwin_display, the_language_row_swaps_the_two_rather_than_setting_one) {
  SimulatedMachine machine;
  SimulatedSensors sensors;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;
  PrepareMenuState saved;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  const uint8_t at_the_start = hmiFlag.language;
  const std::vector<RowEffect> first = walk_the_prepare_menu(knob, pump);
  const uint8_t after_one_pass = hmiFlag.language;
  const std::vector<RowEffect> second = walk_the_prepare_menu(knob, pump);
  panel.finish();

  TEST_ASSERT_EQUAL_MESSAGE(first.size(), second.size(),
    "the two walks should have covered the same menu");
  TEST_ASSERT_NOT_EQUAL_MESSAGE(at_the_start, after_one_pass,
    "one press of the language row should change the language");
  TEST_ASSERT_EQUAL_MESSAGE(at_the_start, hmiFlag.language,
    "and a second press should change it back, so the row is a way in and a way out");
}

#endif // HAS_HOTEND && HAS_HEATED_BED && HAS_PREHEAT

// ---------------------------------------------------------------------------
// Home offsets, and a row in Advanced Settings that does nothing
// ---------------------------------------------------------------------------

#if HAS_HOME_OFFSET

namespace {

  /**
   * Puts the home offsets back.
   *
   * Register #47 is exactly this fixture missing: a test that left a home offset behind moved
   * the origin for every test that ran after it, and the failures surfaced somewhere else
   * entirely. Unity's failure path is a `longjmp`, so the restore has to be a destructor.
   */
  struct HomeOffsets {
    xyz_pos_t was;
    HomeOffsets() : was(motion.home_offset) {}
    ~HomeOffsets() {
      LOOP_NUM_AXES(a) motion.set_home_offset((AxisEnum)a, was[(AxisEnum)a]);
      checkkey = ID_MainMenu;
    }
  };

}

/**
 * The home-offset menu leads to one editor per axis.
 *
 * Pure navigation, so the shared walk is the right instrument here — unlike the Prepare menu
 * above, every row of this one goes somewhere. The failure it catches is the usual transposed
 * pair, and it matters more here than in most menus: the three rows look identical, the editors
 * they open look identical, and the only thing distinguishing them is which axis the committed
 * number lands on. Somebody correcting a Y offset and moving Z instead would find out at the
 * next print, from the nozzle.
 */
MARLIN_TEST(dwin_display, the_home_offset_menu_leads_to_one_editor_per_axis) {
  SimulatedMachine machine;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;
  HomeOffsets saved;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };

  const std::vector<uint8_t> forwards = { ID_AdvSet, ID_HomeOffX, ID_HomeOffY, ID_HomeOffZ };
  const std::vector<uint8_t> seen = walk_a_menu(knob, pump, ID_HomeOff, forwards.size());
  panel.finish();

  the_walk_visited(seen, forwards, "Home Offset");
}

/**
 * The panel edits tenths of a millimetre and the machine stores millimetres.
 *
 * `hmiHomeOffN()` commits `posScaled / 10`, and that single division is the whole relationship
 * between what a person reads off the screen and what the firmware acts on. Lose it and a 0.2 mm
 * correction becomes 2 mm; on Z that is the difference between a nudge and driving the nozzle
 * into the bed, and the panel would go on displaying the number the operator meant.
 *
 * Two values rather than one, because a single point cannot tell a scale factor from an offset:
 * a driver that stored `posScaled - 225` would satisfy any test that only edited 250. Two points
 * fix both, and the ratio is the claim.
 *
 * The other axes are asserted unchanged in the same breath. A driver that wrote every edit into
 * X would pass every assertion above about X, and the transposition is the failure this menu is
 * most prone to — three rows that differ only in which field they touch.
 */
MARLIN_TEST(dwin_display, the_home_offset_editor_stores_millimetres_for_the_axis_it_names) {
  SimulatedMachine machine;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;
  HomeOffsets saved;

  const auto pump = [] { dwinHandleScreen(); };

  LOOP_NUM_AXES(a) motion.set_home_offset((AxisEnum)a, 0);

  // Two points on X, which pin the scale and rule out a constant offset.
  checkkey = ID_HomeOffX; hmiValues.homeOffsScaled.x = 250; knob.click(pump);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(25.0f, motion.home_offset.x,
    "the panel shows tenths of a millimetre and the machine should store millimetres");

  checkkey = ID_HomeOffX; hmiValues.homeOffsScaled.x = 100; knob.click(pump);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(10.0f, motion.home_offset.x,
    "and the same division at a second value, so this is a ratio and not a coincidence");

  #if HAS_Z_AXIS
    // Z has its own row, its own field and its own tighter limit, so it needs its own point.
    checkkey = ID_HomeOffZ; hmiValues.homeOffsScaled.z = 15; knob.click(pump);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.5f, motion.home_offset.z,
      "and the Z row should store Z");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(10.0f, motion.home_offset.x,
      "without disturbing the axis edited before it");
  #endif
  #if HAS_Y_AXIS
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, motion.home_offset.y,
      "and an axis never edited should still be where it started");
  #endif

  panel.finish();
}

/**
 * Each axis is clamped to its own range, and Z's is the tight one.
 *
 * `hmiHomeOffN()` takes its bounds as arguments — `±500` for X and Y, `±20` for Z, in the tenths
 * the panel edits — so the three editors are one function called three ways and the only thing
 * separating them is those numbers. Fifty millimetres of home offset on X moves the origin
 * across the bed, which is recoverable and obvious. Fifty on Z drives the nozzle through the
 * bed, which is neither. That is why Z gets a twenty-fifth of the range, and it is the kind of
 * asymmetry that a later edit "tidies up" into one shared constant.
 *
 * The mutation run is what asked for this: thirty-six survivors sat on those three lines, because
 * `LIMIT()` runs only on the *turning* path and the editor test beside this one commits without
 * ever turning. Coverage could not see the difference; every one of those mutants ran.
 *
 * **How far the knob has to turn is not asserted, and must not be.** `encoderMoveValue` is
 * rate-multiplied — a fast turn counts for a hundred detents — so a fixed number of turns would
 * be pinning the encoder's acceleration curve rather than the limit. The test winds until the
 * value stops moving, which is where the clamp is whatever the step size happens to be.
 *
 * Both ends of each range, because a clamp written `LIMIT(v, lo, lo)` or `LIMIT(v, hi, hi)` would
 * satisfy a test that only pushed one way.
 */
MARLIN_TEST(dwin_display, each_home_offset_axis_is_clamped_to_its_own_range) {
  SimulatedMachine machine;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;
  HomeOffsets saved;

  const auto pump = [] { dwinHandleScreen(); };

  // Wind one way until the edited value stops moving — that is the clamp — then commit.
  const auto wind_to_a_clamp_and_commit = [&](const uint8_t screen, float &field,
                                              const bool one_way) {
    field = 0;
    checkkey = screen;
    float last = field;
    for (uint16_t i = 0; i < 4000; i++) {
      if (one_way) knob.turn_clockwise(pump); else knob.turn_counterclockwise(pump);
      if (field == last) break;
      last = field;
    }
    checkkey = screen;
    knob.click(pump);
  };

  /**
   * Both ends, reported low-then-high rather than by direction.
   *
   * Which way the fixture's "clockwise" drives the value is an agreement between the stand-in
   * and the driver, not a claim about the machine — the first version of this test asserted
   * `+2` for a clockwise wind and got `-2`, which is the fifth time that assumption has been
   * wrong in this file. What survives rewiring the encoder is that the range has two ends and
   * where they are.
   */
  const auto range_of = [&](const uint8_t screen, float &field) {
    wind_to_a_clamp_and_commit(screen, field, true);
    const float a = field / 10;
    wind_to_a_clamp_and_commit(screen, field, false);
    const float b = field / 10;
    return std::pair<float, float>(_MIN(a, b), _MAX(a, b));
  };

  LOOP_NUM_AXES(a) motion.set_home_offset((AxisEnum)a, 0);

  #if HAS_Z_AXIS
    const auto z = range_of(ID_HomeOffZ, hmiValues.homeOffsScaled.z);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-2.0f, z.first,
      "Z should stop at -2 mm however far the knob is turned");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f, z.second,
      "and at +2 mm the other way, so the range is bounded at both ends");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(z.second, motion.home_offset.z,
      "and the value the panel settled on is the one the machine stored");
  #endif

  // X is the comparison that makes Z's limit a decision rather than a global constant.
  const auto x = range_of(ID_HomeOffX, hmiValues.homeOffsScaled.x);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-50.0f, x.first,
    "X should reach -50 mm - the limits are per axis, and X's is 25 times Z's");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(50.0f, x.second, "and +50 mm the other way");

  panel.finish();
}

#if HAS_HEATED_BED && DISABLED(PIDTEMPBED)

/**
 * LEGACY-BEHAVIOR: defect #56 — Advanced Settings draws a "Bed PID" row that does nothing.
 *
 * The row's position and the row's behaviour are decided by two different conditions, and they
 * disagree. `ADVSET_CASE_BEDPID` is `ADVSET_CASE_HEPID + ENABLED(HAS_HEATED_BED)` — keyed on
 * *having* a bed — and `itemAdvBedPID()` is drawn with no guard at all, while the arm that acts
 * on it is `#if ENABLED(PIDTEMPBED)` — keyed on *regulating* the bed with PID. A machine with a
 * bed switched on a thermostat, which is Marlin's default and every configuration in `test/`
 * that builds this driver, therefore lists a menu item that cannot do anything.
 *
 * The same mismatch exists one row up, between `HAS_HOTEND` and `PIDTEMP`.
 *
 * What a person experiences is a control that does not work: they select Bed PID, press, and the
 * panel sits there. There is no message, no progress screen, nothing to distinguish it from a
 * dead encoder — and the next thing anyone does with a dead control is press it harder. Register
 * #56.
 *
 * The test presses the *far clamp* rather than walking, deliberately. One row below it is Nozzle
 * PID, whose arm **is** compiled and starts a ten-cycle autotune that would rewrite the hotend's
 * gains for every test after it. Winding to the clamp passes over that row without pressing it.
 * The guard on this test is the defect's own precondition, so a configuration that fixed the
 * mismatch by enabling `PIDTEMPBED` would stop running it rather than start failing it.
 */
MARLIN_TEST(dwin_display, the_advanced_settings_menu_offers_a_row_that_does_nothing) {
  SimulatedMachine machine;
  SerialCapture panel(LCD_SERIAL);
  SimulatedEncoder knob;

  ui.backlight = true;
  marlin.wait_for_user = false;
  HomeOffsets saved;

  const auto pump = [] { HAL_test_advance_millis(ENCODER_WAIT_MS + 1); dwinHandleScreen(); };
  constexpr uint8_t ROOM = 16;

  // Which clamp is Back? Free to ask: pressing Back only returns to the Control menu, and
  // pressing the far clamp is the inert row this test is about.
  checkkey = ID_AdvSet;
  for (uint8_t i = 0; i < ROOM; i++) { knob.turn_clockwise(pump); checkkey = ID_AdvSet; }
  knob.click(pump);
  const bool back_is_clockwise = (checkkey == ID_Control);

  const auto wind = [&](const bool towards_back) {
    checkkey = ID_AdvSet;
    for (uint8_t i = 0; i < ROOM; i++) {
      if (towards_back == back_is_clockwise) knob.turn_clockwise(pump);
      else knob.turn_counterclockwise(pump);
      checkkey = ID_AdvSet;
    }
  };

  // The menu responds at all: Back leaves it, and the row next to Back opens the offsets.
  wind(true);
  knob.click(pump);
  TEST_ASSERT_EQUAL_MESSAGE(ID_Control, checkkey,
    "the row at one clamp should be Back, which is what anchors the rest of this test");

  wind(true);
  checkkey = ID_AdvSet;
  if (back_is_clockwise) knob.turn_counterclockwise(pump); else knob.turn_clockwise(pump);
  knob.click(pump);
  TEST_ASSERT_EQUAL_MESSAGE(ID_HomeOff, checkkey,
    "and the row beside it should open the home offsets, so presses are being delivered");

  // ...and the row at the other end is not.
  wind(false);
  knob.click(pump);
  panel.finish();

  TEST_ASSERT_EQUAL_MESSAGE(ID_AdvSet, checkkey,
    "the last row of Advanced Settings is drawn but has no arm compiled, so pressing it "
    "leaves the panel exactly where it was (defect #56)");
}

#endif // HAS_HEATED_BED && DISABLED(PIDTEMPBED)

#endif // HAS_HOME_OFFSET

// ---------------------------------------------------------------------------
// The position readout
// ---------------------------------------------------------------------------

/**
 * An axis the machine has not homed is shown as question marks, not as a number.
 *
 * This is the most consequential thing on the status bar. A coordinate implies the machine
 * knows where the tool is; before homing it does not, and the number it would print is whatever
 * the counters happened to hold. Somebody reading `0.0` off an unhomed Z and lowering the
 * nozzle "just a little" is the failure this display exists to prevent.
 *
 * `_update_axis_value()` decides per axis, from `axis_should_home()`, and blinks the marks so
 * they cannot be mistaken for a reading. The readout is reached through `updateVariable()` —
 * `_draw_xyz_position()` is file-scope in the driver — and it only redraws when the blink phase
 * turns over, which is why the test walks the clock rather than calling twice.
 *
 * The assertion is on the characters in the byte stream, not on where they were drawn: `???`
 * is content the panel was told to display, while the coordinates and font ids around it are
 * protocol this test has no business pinning.
 */
MARLIN_TEST(dwin_display, an_axis_that_has_not_been_homed_is_shown_as_question_marks) {
  SimulatedMachine machine;
  SimulatedSensors sensors;

  checkkey = ID_MainMenu;

  motion.set_all_unhomed();

  // The readout redraws when `millis() & 0x400` turns over, which is about every second of
  // simulated time. Walk until it has flipped *and* is set, since the marks are drawn on the
  // blink-on half.
  std::string drawn;
  for (uint16_t i = 0; i < 64 && drawn.find("???") == std::string::npos; i++) {
    SerialCapture panel(LCD_SERIAL);
    HAL_test_advance_millis(256);
    updateVariable();
    drawn = panel.finish();
  }

  TEST_ASSERT_TRUE_MESSAGE(drawn.find("???") != std::string::npos,
    "an unhomed axis should be shown as question marks rather than a coordinate");

  motion.set_all_homed();
}

/**
 * ...and once it is homed it shows a number.
 *
 * The other arm, and the one that says the marks are a *statement about trust* rather than the
 * only thing the readout can draw. Without it, a driver that printed `???.?` for every axis for
 * ever would pass the test above and look, to anyone reading the code, entirely correct.
 */
MARLIN_TEST(dwin_display, a_homed_axis_is_shown_as_a_position) {
  SimulatedMachine machine;
  SimulatedSensors sensors;

  checkkey = ID_MainMenu;
  motion.set_all_homed();

  // Let a few blink periods pass so the readout has certainly been redrawn since homing.
  std::string drawn;
  for (uint16_t i = 0; i < 16; i++) {
    SerialCapture panel(LCD_SERIAL);
    HAL_test_advance_millis(256);
    updateVariable();
    const std::string chunk = panel.finish();
    if (!chunk.empty()) drawn += chunk;
  }

  TEST_ASSERT_TRUE_MESSAGE(drawn.find("???") == std::string::npos,
    "a homed axis should be shown as a position, not as question marks");
}

// ---------------------------------------------------------------------------
// What the file menu calls a file
// ---------------------------------------------------------------------------

#if HAS_MEDIA

namespace {

  /**
   * Run a filename through the menu's namer and return what the row would say.
   *
   * The length is passed rather than left to `MENU_CHAR_LIMIT`, because the truncation rule is
   * the same on every panel while the room to print it is not — the same reasoning as
   * `MAX_MESSAGE_SIZE` in the message-template tests.
   *
   * The card's own `longFilename` has to be set as well as the argument, because the function
   * takes its starting index from `card.longest_filename()` rather than from `src`. That is
   * defect #54 and it is why this helper writes the same string into both.
   */
  std::string menu_name_for(const char * const filename, const bool is_dir, const size_t maxlen) {
    strlcpy(card.longFilename, filename, sizeof(card.longFilename));
    card.flag.filenameIsDir = is_dir;

    char src[LONG_FILENAME_LENGTH];
    strlcpy(src, filename, sizeof(src));

    char dst[LONG_FILENAME_LENGTH] = { 0 };
    make_name_without_ext(dst, src, maxlen);
    return std::string(dst);
  }

}

/**
 * A file loses its extension; a folder keeps its dots.
 *
 * The menu shows `model` rather than `model.gcode` because the row is narrow and every file on
 * the card ends the same way. A *folder* has no extension to remove, so the same trimming
 * applied to it would eat part of its name — `v1.2` would become `v1`, naming a directory that
 * is not there.
 *
 * Both directions, because the guard is a single `if` on `filenameIsDir` and a machine that
 * ignored it would look right on every file and wrong on every dotted folder.
 */
MARLIN_TEST(dwin_display, the_file_menu_drops_an_extension_but_not_a_folders_dots) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("model", menu_name_for("model.gcode", false, 24).c_str(),
    "a file should be shown without its extension");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("part", menu_name_for("part.g", false, 24).c_str(),
    "whichever of the three extensions it has");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("v1.2", menu_name_for("v1.2", true, 24).c_str(),
    "a folder has no extension to drop and should keep its dots");
}

/**
 * Only the last dot counts.
 *
 * Names with a version or a date in them are ordinary — `bracket.v2.gcode` — and the search
 * runs backwards from the end for exactly that reason. A machine that stopped at the first dot
 * would show `bracket`, so two revisions of the same part would be indistinguishable in the
 * list you pick a print from.
 */
MARLIN_TEST(dwin_display, only_the_last_dot_starts_the_extension) {
  TEST_ASSERT_EQUAL_STRING("bracket.v2", menu_name_for("bracket.v2.gcode", false, 24).c_str());
}

/**
 * LEGACY-BEHAVIOR: defect #55 — a file whose name has no dot is shown as an empty row.
 *
 * The backwards search is `while (pos && src[pos] != '.') pos--`. With nothing to find it walks
 * all the way to zero, and zero is then taken as the length: `dst[0] = '\0'`. The row is blank.
 *
 * **Latent, not live**, and the reason is in the card rather than here: `is_visible_entity()`
 * only lists a file when its 8.3 extension begins with `G`, and the 8.3 name is derived from
 * the long one — so a file with no extension never reaches the menu. Directories are handled
 * before the loop by the `filenameIsDir` guard, which is why `v1.2` survives above.
 *
 * Pinned rather than fixed, and pinned to what it *does*: the first version of this test
 * asserted the sensible answer, `README`, and found the empty string. That is the finding. If
 * the card's filter ever widens, or a caller passes a name from somewhere else, this test is
 * what will notice.
 */
MARLIN_TEST(dwin_display, a_name_with_no_extension_comes_out_empty) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", menu_name_for("README", false, 24).c_str(),
    "a dotless name walks the search to zero and is shown as nothing (defect #55)");

  // The same name as a folder is left whole, which is the arm that makes this a defect in the
  // file path rather than a deliberate rule about dotless names.
  TEST_ASSERT_EQUAL_STRING_MESSAGE("README", menu_name_for("README", true, 24).c_str(),
    "a folder skips the extension search entirely and keeps its name");
}

/**
 * A name too long for the row is cut and marked, and the mark replaces rather than extends.
 *
 * The three dots are written *into* the last three characters of the allowance, not appended
 * after it — `dst[--pos] = '.'` three times from `len = maxlen`. So the result is exactly
 * `maxlen` characters however long the name was, which is what stops the row overrunning into
 * whatever the panel draws next. Asserting the ellipsis without asserting the length would pass
 * against an implementation that appended it.
 *
 * Bracketed: a name that fits exactly is untouched, and one character more is cut.
 */
MARLIN_TEST(dwin_display, a_name_too_long_for_the_row_is_cut_and_marked) {
  const std::string fits = menu_name_for("12345678.gcode", false, 8);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("12345678", fits.c_str(),
    "a name that fills the row exactly should be left alone");

  const std::string cut = menu_name_for("123456789.gcode", false, 8);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("12345...", cut.c_str(),
    "one character more should be cut and marked with an ellipsis");
  TEST_ASSERT_EQUAL_MESSAGE(8, cut.size(),
    "and the marked name should still be exactly the width of the row");
}

#endif // HAS_MEDIA

#endif // ENABLED(DWIN_CREALITY_LCD)
