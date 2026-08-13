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
 * M0/M1 — stop and wait for the user.
 *
 * `Marlin::wait_for_user_response()` spins on `idle()` until `wait_for_user` clears or, if
 * a P/S wait was given, until that much time has passed. Under the test HAL, `idle()` costs
 * simulated time (the same mechanism `test_blocking_commands.cpp` uses for G4), so the P/S
 * path returns deterministically. There is no way from a single-threaded test to clear
 * `wait_for_user` early — that is M108, delivered on a real board through the emergency
 * parser reading the UART directly, not through the command queue `idle()` drains — so
 * every scenario here gives a P or S limit and never exercises the unbounded wait.
 *
 * `EXTENSIBLE_UI` is the observation seam: `ExtUI::onUserConfirmRequired` is what a screen
 * is told, and `stub_extui.h` records it rather than discarding it.
 */

#include "src/inc/MarlinConfig.h"

#if ENABLED(EXTENSIBLE_UI)

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/stub_extui.h"
#include "serial_capture.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/module/stepper.h"

#include <string.h>

namespace {

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    SerialCapture capture;
    parser.parse(buf);
    gcode.process_parsed_command(true);
    (void)capture.finish();
  }

  struct WatchingDisplay {
    WatchingDisplay() { RecordedUI::reset(); }
    ~WatchingDisplay() { RecordedUI::reset(); }
  };

}

// With no message, the display is asked to confirm with the standard wait prompt.
MARLIN_TEST(m0_m1, M0_with_no_message_asks_the_display_to_confirm_with_the_default_prompt) {
  SimulatedMachine machine;
  WatchingDisplay display;

  host_sends("M0 P50");

  TEST_ASSERT_TRUE_MESSAGE(RecordedUI::confirms_required > 0,
    "the display should have been asked to confirm");
  TEST_ASSERT_EQUAL_STRING("Click to Resume...", RecordedUI::last_confirm.c_str());
}

// With a message, that text reaches the display instead of the standard prompt.
MARLIN_TEST(m0_m1, M1_with_a_message_shows_it_to_the_display_instead) {
  SimulatedMachine machine;
  WatchingDisplay display;

  host_sends("M1 S1 come back soon");

  TEST_ASSERT_EQUAL_STRING("come back soon", RecordedUI::last_confirm.c_str());
}

// P is milliseconds, and M0 waits at least that long before returning — the same bracket
// test_blocking_commands.cpp uses for G4 P.
MARLIN_TEST(m0_m1, M0_P_waits_for_at_least_that_many_milliseconds) {
  SimulatedMachine machine;
  WatchingDisplay display;

  const millis_t before = millis();
  host_sends("M0 P75");
  TEST_ASSERT_TRUE(millis() - before >= 75);
}

// S is seconds, not milliseconds.
MARLIN_TEST(m0_m1, M1_S_waits_in_seconds_not_milliseconds) {
  SimulatedMachine machine;
  WatchingDisplay display;

  const millis_t before = millis();
  host_sends("M1 S1");
  TEST_ASSERT_TRUE(millis() - before >= 1000);
}

// M0/M1 synchronizes before it waits, so a queued move finishes even though the P given is
// far shorter than the move — the same shape as
// blocking_commands___G4_finishes_queued_moves_before_dwelling.
MARLIN_TEST(m0_m1, M0_finishes_a_queued_move_before_it_starts_waiting) {
  SimulatedMachine machine;
  WatchingDisplay display;

  xyze_pos_t origin = { 0 };
  motion.position = origin;
  planner.set_position_mm(origin);

  xyze_pos_t target = { 0 }; target.x = 1.0f;
  TEST_ASSERT_TRUE(planner.buffer_line(target, 10.0f)); // 1 mm at 10 mm/s = 100 ms

  host_sends("M0 P1"); // far shorter than the move it must wait out first

  TEST_ASSERT_FALSE(planner.has_blocks_queued());
  TEST_ASSERT_EQUAL(80, stepper.position(X_AXIS)); // 1 mm at 80 steps/mm
}

// With a message, the host hears the message — not the stock "M0 Stop"/"M1 Stop" reply
// that only stands in for one. The one place `parser.string_arg` gates what the host is
// told, distinct from the display-facing checks above.
MARLIN_TEST(m0_m1, M1_with_a_message_tells_the_host_the_message_not_the_stock_reply) {
  SimulatedMachine machine;
  WatchingDisplay display;
  static char buf[48];
  strncpy(buf, "M1 P10 hello host", sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  SerialCapture capture;
  parser.parse(buf);
  gcode.process_parsed_command(true);
  const std::string out = capture.finish();

  TEST_ASSERT_TRUE_MESSAGE(out.find("hello host") != std::string::npos,
    "the host should have been told the message");
  TEST_ASSERT_TRUE_MESSAGE(out.find("M1 Stop") == std::string::npos,
    "not the stock reply");
}

// The host is told which command stopped it, when there is no message to show instead —
// the one place `parser.codenum` is read in this file. `//action:prompt_begin` carries the
// literal text, so the M0/M1 distinction is only visible on the serial channel it went out
// on, not through the display.
MARLIN_TEST(m0_m1, M0_tells_the_host_M0_stopped_it) {
  SimulatedMachine machine;
  WatchingDisplay display;
  static char buf[32];
  strncpy(buf, "M0 P10", sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  SerialCapture capture;
  parser.parse(buf);
  gcode.process_parsed_command(true);
  const std::string out = capture.finish();

  TEST_ASSERT_TRUE_MESSAGE(out.find("M0 Stop") != std::string::npos,
    "the host should have been told M0 stopped it");
  TEST_ASSERT_TRUE_MESSAGE(out.find("M1 Stop") == std::string::npos,
    "and not told M1");
}

MARLIN_TEST(m0_m1, M1_tells_the_host_M1_stopped_it) {
  SimulatedMachine machine;
  WatchingDisplay display;
  static char buf[32];
  strncpy(buf, "M1 P10", sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  SerialCapture capture;
  parser.parse(buf);
  gcode.process_parsed_command(true);
  const std::string out = capture.finish();

  TEST_ASSERT_TRUE_MESSAGE(out.find("M1 Stop") != std::string::npos,
    "the host should have been told M1 stopped it");
  TEST_ASSERT_TRUE_MESSAGE(out.find("M0 Stop") == std::string::npos,
    "and not told M0");
}

#endif // ENABLED(EXTENSIBLE_UI)
