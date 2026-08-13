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
 * Stopping and waiting for a person.
 *
 * `M0` and `M1` hold the machine until somebody presses the button, or until a period
 * given on the command line runs out. Three configurations compile this and, until now,
 * none of them ran it — which is the whole reason it sat at 0%: not neglect, an absence
 * of any test that could reach a command whose entire job is to not return.
 *
 * Under the test HAL it can be reached, because `marlin.idle()` costs simulated time, so
 * a timed wait ends for the same reason it ends on a board. Test-HAL only: these
 * deliberately hang under HAL/LINUX.
 *
 * Which message goes where depends on the display the build has, and the assertions
 * follow that rather than pretending it does not matter — an ExtUI build is *told*, and
 * a build with no menu at all writes the text to the host. The timing half is the same
 * everywhere and is asserted unguarded.
 */


#include "../test/unit_tests.h"
#include "src/inc/MarlinConfig.h"

#if HAS_RESUME_CONTINUE

#include "../support/simulated_machine.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/MarlinCore.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "serial_capture.h"
#include <string.h>
#include <thread>
#include <chrono>

#if ENABLED(EXTENSIBLE_UI)
#include "../support/stub_extui.h"
#endif

namespace {

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  // How long a command took, in simulated milliseconds.
  millis_t elapsed_over(const char * const line) {
    const millis_t before = millis();
    host_sends(line);
    return millis() - before;
  }

}

/**
 * `P` is milliseconds and `S` is seconds, and the wait really lasts that long.
 *
 * Bracketed rather than shown once: a single duration would pass against a wait that
 * ignored the parameter and always ran for some fixed time of its own. Two durations an
 * order of magnitude apart, each asserted from both sides, locate it.
 *
 * The upper bounds are loose on purpose. `idle()` advances the clock in fixed steps and
 * does real work in each one, so a wait overshoots slightly and by an amount that is a
 * property of the harness, not of the firmware.
 */
MARLIN_TEST(user_wait, M0_P_waits_for_the_milliseconds_it_is_given) {
  SimulatedMachine machine;

  const millis_t took = elapsed_over("M0 P100");
  TEST_ASSERT_TRUE_MESSAGE(took >= 100, "M0 P100 should not return before its period is up");
  TEST_ASSERT_TRUE_MESSAGE(took < 200, "and should not wait appreciably longer than it");
}

MARLIN_TEST(user_wait, M0_S_waits_for_the_seconds_it_is_given) {
  SimulatedMachine machine;

  const millis_t took = elapsed_over("M0 S1");
  TEST_ASSERT_TRUE_MESSAGE(took >= 1000, "M0 S1 should wait a whole second");
  TEST_ASSERT_TRUE_MESSAGE(took < 1200, "and not appreciably longer");
}

// The later parameter wins, which is what makes S seconds rather than an alias for P.
MARLIN_TEST(user_wait, S_is_read_as_seconds_beside_a_P_in_milliseconds) {
  SimulatedMachine machine;

  const millis_t took = elapsed_over("M0 P10 S1");
  TEST_ASSERT_TRUE_MESSAGE(took >= 1000,
    "the S is read after the P, so a second beats ten milliseconds");
}

// M1 is the conditional stop and waits the same way.
MARLIN_TEST(user_wait, M1_P_waits_like_M0) {
  SimulatedMachine machine;

  const millis_t took = elapsed_over("M1 P100");
  TEST_ASSERT_TRUE_MESSAGE(took >= 100, "M1 P100 should wait its period out too");
  TEST_ASSERT_TRUE_MESSAGE(took < 200, "and no longer");
}

/**
 * A wait with no period at all ends when the person answers, and not before.
 *
 * This is the branch the command exists for, and the one nothing in the suite could reach:
 * with no `P` and no `S` there is no deadline, so the only thing that ends the loop is
 * `marlin.wait_for_user` going false. Every timed test above would pass against an `M0`
 * that never looked at the flag at all.
 *
 * The answer arrives from a second thread, which is the same arrangement `SerialCapture`
 * uses and for the same reason: the firmware is inside a blocking call, so nothing on this
 * thread can act until it returns. The delay is wall-clock rather than simulated, because
 * the *point* is that it lands while the machine is spinning — simulated time only moves
 * when the machine moves it, so this side has to wait in real seconds.
 *
 * Note the assertion is on the clock, not on the flag: `wait_for_user_response()` clears the
 * flag on its way out either way, so a test asserting it was cleared would pass against a
 * loop that never ran. Simulated time only advances inside `idle()`, so the machine having
 * spent time is the evidence that it actually waited.
 */
MARLIN_TEST(user_wait, an_untimed_wait_ends_when_the_user_answers) {
  SimulatedMachine machine;

  marlin.wait_for_user = false;
  const millis_t before = millis();

  std::thread the_person([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    marlin.wait_for_user = false;
  });

  host_sends("M0");
  the_person.join();

  // Not `> before`: that is satisfied by a wait of one millisecond, and a one-millisecond
  // deadline is exactly what an `ms` initialised to 1 instead of 0 would produce. The
  // untimed case has to be shown to outlast any trivial deadline, not merely to take time.
  TEST_ASSERT_TRUE_MESSAGE(millis() - before > 20,
    "an untimed M0 should wait for the answer, not run out a deadline of its own");
  TEST_ASSERT_FALSE(marlin.wait_for_user);
}

/**
 * The queue is emptied before anybody is asked to do anything.
 *
 * `M0` is where a print stops so a person can reach into the machine — change a filament,
 * lift a part off the bed. Asking them while the head is still moving is the one thing it
 * must not do, and the `planner.synchronize()` that prevents it is a bare statement with no
 * return value, so nothing about the message or the timing says whether it ran.
 *
 * Queueing a move that takes ten times the wait makes it say so: with the synchronize the
 * command cannot return until the move is done, and without it the wait is over first.
 */
MARLIN_TEST(user_wait, the_machine_finishes_moving_before_it_asks) {
  SimulatedMachine machine;

  xyze_pos_t origin = { 0 };
  motion.position = origin;
  planner.set_position_mm(origin);

  xyze_pos_t target = { 0 }; target.x = 10.0f;
  TEST_ASSERT_TRUE(planner.buffer_line(target, 10.0f));   // 10 mm at 10 mm/s — about a second
  TEST_ASSERT_TRUE(planner.has_blocks_queued());

  const millis_t took = elapsed_over("M0 P100");

  TEST_ASSERT_FALSE_MESSAGE(planner.has_blocks_queued(),
    "the move should be finished by the time M0 returns");
  TEST_ASSERT_TRUE_MESSAGE(took > 900,
    "and M0 should have waited for it rather than timing out its own hundred milliseconds");
}

#if ENABLED(EXTENSIBLE_UI) && DISABLED(DWIN_LCD_PROUI) && !HAS_MARLINUI_MENU

/**
 * A display is told that somebody has to confirm, and told what to say.
 *
 * The contract of ExtUI is that the firmware tells the display when something happens, so
 * silence is the failure that matters here — `stub_extui` records rather than discards for
 * exactly this reason.
 */
MARLIN_TEST(user_wait, the_display_is_asked_to_confirm_with_the_message_given) {
  SimulatedMachine machine;
  RecordedUI::reset();

  host_sends("M0 P100 Take the part off");

  TEST_ASSERT_EQUAL_MESSAGE(1, RecordedUI::confirms_required,
    "the display should have been asked for a confirmation exactly once");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Take the part off", RecordedUI::last_confirm.c_str(),
    "and told the words from the command line, not a default");
}

MARLIN_TEST(user_wait, a_bare_stop_asks_with_the_firmware_s_own_wording) {
  SimulatedMachine machine;
  RecordedUI::reset();

  host_sends("M0 P100");

  TEST_ASSERT_EQUAL_MESSAGE(1, RecordedUI::confirms_required,
    "a stop with nothing to say should still ask for a confirmation");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Click to Resume...", RecordedUI::last_confirm.c_str(),
    "using the firmware's own wording");
}

#elif !HAS_MARLINUI_MENU && DISABLED(DWIN_LCD_PROUI)

/**
 * With no menu to put it on, the message goes to the host instead.
 *
 * Asserted on the channel as well as the words: this is an `echo:`, not an error, and the
 * two are separate reports that a test searching for the text alone cannot tell apart.
 */
MARLIN_TEST(user_wait, the_message_is_echoed_to_the_host_when_there_is_no_menu) {
  SimulatedMachine machine;
  SerialCapture host;

  host_sends("M0 P100 Take the part off");

  TEST_ASSERT_TRUE_MESSAGE(host.saw("echo:Take the part off"),
    "the words from the command line should be echoed to the host");
}

// Nothing to say, nothing said — the echo is the string argument or it is absent.
MARLIN_TEST(user_wait, a_bare_stop_echoes_nothing) {
  SimulatedMachine machine;
  SerialCapture host;

  host_sends("M0 P100");

  TEST_ASSERT_FALSE_MESSAGE(host.saw("Click to Resume"),
    "with no menu and no message on the command line there is nothing to echo");
}

#endif

#if ENABLED(HOST_PROMPT_SUPPORT)

/**
 * The host is told which command stopped it, and what it said.
 *
 * A host driving the machine gets an `//action:` prompt so it can put a Continue button in
 * front of the operator. What goes on it is the message from the command line if there was
 * one, and otherwise the name of the command that stopped — which is the only place in this
 * file where `M0` and `M1` behave differently at all.
 *
 * Asserted on the whole prompt rather than on the words: `M0 Stop` and `M1 Stop` differ by
 * one character, and a test looking for "Stop" would pass against either.
 */
MARLIN_TEST(user_wait, the_host_prompt_names_the_command_that_stopped) {
  SimulatedMachine machine;

  {
    SerialCapture host;
    host_sends("M0 P100");
    TEST_ASSERT_TRUE_MESSAGE(host.saw("prompt_begin M0 Stop"),
      "an M0 with nothing to say should name itself in the prompt");
  }
  {
    SerialCapture host;
    host_sends("M1 P100");
    TEST_ASSERT_TRUE_MESSAGE(host.saw("prompt_begin M1 Stop"),
      "and an M1 should name itself, which is the only thing that tells the two apart");
  }
}

MARLIN_TEST(user_wait, the_host_prompt_carries_the_message_from_the_command_line) {
  SimulatedMachine machine;
  SerialCapture host;

  host_sends("M0 P100 Take the part off");

  const std::string &sent = host.finish();
  TEST_ASSERT_TRUE_MESSAGE(sent.find("prompt_begin Take the part off") != std::string::npos,
    "a message on the command line should be what the host puts in front of the operator");
  TEST_ASSERT_TRUE_MESSAGE(sent.find("M0 Stop") == std::string::npos,
    "and it should replace the command's own name, not sit beside it");
}

#endif // HOST_PROMPT_SUPPORT

#endif // HAS_RESUME_CONTINUE
