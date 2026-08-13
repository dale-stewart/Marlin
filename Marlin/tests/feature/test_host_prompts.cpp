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
 * Talking to the host program.
 *
 * When a printer needs a person — change the filament, take the part off, confirm before
 * it carries on — and there is no screen on the machine, it asks through the host. The
 * host is OctoPrint or Pronterface or a slicer's terminal, and the agreement between them
 * is a line protocol: `//action:` followed by a verb.
 *
 * A prompt is not one line, it is a **sequence**: end whatever was showing, begin the new
 * one with its text, name each button, then show it. A host builds a dialogue box by
 * reading those in order, so the order is the behaviour — a button announced after `show`
 * is a button nobody can press. Almost everything below is therefore an assertion about
 * sequence rather than about content, which is what a test of a protocol has to be.
 *
 * The other half is the reply. `M876 S<n>` is the button coming back, and what it means
 * depends on the question that was asked — the same `S1` resumes a wait or dismisses a
 * runout depending on what the firmware last put up. That routing is the part with real
 * consequences and it had no test at all.
 */

#include "../test/unit_tests.h"
#include "src/inc/MarlinConfig.h"

#if ENABLED(HOST_PROMPT_SUPPORT)

#include "src/feature/host_actions.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/MarlinCore.h"
#include "../gcode/serial_capture.h"
#include <string.h>
#include <stdio.h>

namespace {

  /**
   * The prompt machinery keeps one piece of state — the reason the last prompt was
   * raised — and it is a static that outlives any test. Left set, it changes what the
   * *next* test's `M876` does, which is the between-tests leak that this suite has been
   * bitten by three times now. Cleared both ways round.
   */
  struct QuietHost {
    QuietHost() { hostui.host_prompt_reason = PROMPT_NOT_DEFINED; }
    ~QuietHost() { hostui.host_prompt_reason = PROMPT_NOT_DEFINED; }
  };

  void host_sends(const char * const line) {
    static char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  // Where a substring starts, or npos. Used to compare positions rather than presence:
  // "did it say X" is a much weaker question than "did it say X before Y".
  size_t at(const std::string &s, const char * const needle) { return s.find(needle); }

}

/**
 * Everything this file emits carries the `//action:` prefix, and that prefix is the whole
 * agreement — a host filters its terminal on it. Asserted on a bare action rather than on
 * a prompt, so it fails for one reason.
 */
MARLIN_TEST(host_prompts, every_message_is_prefixed_so_a_host_can_pick_it_out) {
  QuietHost quiet;
  SerialCapture host;

  hostui.action(F("wibble"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "//action:wibble") != std::string::npos,
    "an action should reach the host prefixed and named");
}

/**
 * The `eol` argument decides whether the caller is finished with the line.
 *
 * It exists so that `prompt()` can write `//action:prompt_` and have the *type* follow on
 * the same line. Every mutant of the `if (eol)` survived the whole suite before this,
 * because nothing ever asked for a line that stayed open — and a protocol where a verb
 * silently gains a newline in the middle is one no host can parse.
 */
MARLIN_TEST(host_prompts, an_action_that_is_not_ended_leaves_the_line_open_for_the_caller) {
  QuietHost quiet;
  SerialCapture host;

  hostui.action(F("first"), false);
  hostui.action(F("second"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "//action:first//action:second") != std::string::npos,
    "an unterminated action should be followed on the same line, not on the next one");
}

/**
 * ...and an action that *is* ended starts the next one on a fresh line.
 *
 * The pair matters, not either half. Asserting only that an unterminated action runs on
 * leaves `if (eol)` killable in one direction: a mutant that never writes the newline
 * satisfies it too, because then *everything* runs together. Both directions, or neither.
 */
MARLIN_TEST(host_prompts, an_action_that_is_ended_starts_the_next_one_on_its_own_line) {
  QuietHost quiet;
  SerialCapture host;

  hostui.action(F("first"));
  hostui.action(F("second"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "//action:first\n") != std::string::npos,
    "a terminated action should end its line");
  TEST_ASSERT_TRUE_MESSAGE(at(sent, "first//action:second") == std::string::npos,
    "so the next one cannot run on from it");
}

/**
 * A prompt is four messages in one order, and the order is the point.
 *
 * `end` first, so a host that is already showing something replaces it rather than
 * stacking; then `begin` with the words; then the buttons; then `show`, which is the host's
 * cue that the dialogue is complete. A button emitted after `show` is a button that never
 * appears, and nothing about the individual lines would say so.
 */
MARLIN_TEST(host_prompts, a_prompt_is_ended_begun_buttoned_and_only_then_shown) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_do(PROMPT_INFO, F("Change the filament"), F("Purge"), F("Continue"));
  const std::string sent = host.finish();

  const size_t ended  = at(sent, "prompt_end"),
               begun  = at(sent, "prompt_begin Change the filament"),
               first  = at(sent, "prompt_button Purge"),
               second = at(sent, "prompt_button Continue"),
               shown  = at(sent, "prompt_show");

  TEST_ASSERT_TRUE_MESSAGE(ended  != std::string::npos, "a prompt should close whatever was showing");
  TEST_ASSERT_TRUE_MESSAGE(begun  != std::string::npos, "and announce itself with its message");
  TEST_ASSERT_TRUE_MESSAGE(first  != std::string::npos, "and name its first button");
  TEST_ASSERT_TRUE_MESSAGE(second != std::string::npos, "and its second");
  TEST_ASSERT_TRUE_MESSAGE(shown  != std::string::npos, "and finally ask for it to be shown");

  TEST_ASSERT_TRUE_MESSAGE(ended < begun,  "the old prompt must be closed before the new one opens");
  TEST_ASSERT_TRUE_MESSAGE(begun < first,  "the message must arrive before its buttons");
  TEST_ASSERT_TRUE_MESSAGE(first < second, "and the buttons in the order they were given");
  TEST_ASSERT_TRUE_MESSAGE(second < shown, "and every button before the show, or it is never drawn");
}

// A prompt with one button names one, not two — the second is optional and absent, and
// `_prompt_show` has a guard for each that nothing distinguished.
MARLIN_TEST(host_prompts, a_prompt_with_one_button_names_only_that_one) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_do(PROMPT_INFO, F("Carry on?"), F("Continue"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_button Continue") != std::string::npos,
    "the button that was given should be named");

  size_t count = 0, from = 0;
  while ((from = sent.find("prompt_button", from)) != std::string::npos) { count++; from++; }
  TEST_ASSERT_EQUAL_MESSAGE(1, count, "and no button that was not given");
}

MARLIN_TEST(host_prompts, a_prompt_with_no_buttons_is_just_a_message) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_do(PROMPT_INFO, F("Working"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_begin Working") != std::string::npos,
    "the message should still be sent");
  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_button") == std::string::npos,
    "but a prompt given no buttons should offer none");
  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_show") != std::string::npos,
    "and it should still be shown");
}

/**
 * The verb and its argument share a line; the closing `end` does not.
 *
 * `prompt()` writes `//action:prompt_` and leaves the line open so the type follows, and
 * `prompt_plus()` then puts the text on the same line. `prompt_end` has nothing to add, so
 * it closes. Getting that wrong in either direction gives a host either a verb with no
 * argument or two verbs it reads as one.
 */
MARLIN_TEST(host_prompts, the_closing_end_is_a_line_of_its_own) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_end();
  hostui.prompt_end();
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_end\n") != std::string::npos,
    "an end should close its line");
  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_endprompt_end") == std::string::npos,
    "so two of them cannot arrive as one word");
}

/**
 * A message the firmware built at runtime reaches the host as itself.
 *
 * Nearly every prompt in the firmware is a compile-time string, and on the embedded targets
 * that means it lives in program memory and is read with a different instruction. So there
 * are two paths through `prompt_plus()` chosen by a flag, and the runtime one is the rarer:
 * `M0`'s message from the command line and the pause menu's tool name go that way. Covering
 * only the common path leaves the branch itself unasserted, and on hardware the wrong arm
 * reads a pointer as though it were an address in the other memory space.
 */
MARLIN_TEST(host_prompts, a_message_built_at_runtime_reaches_the_host_intact) {
  QuietHost quiet;
  SerialCapture host;

  char built[32];
  snprintf(built, sizeof(built), "Filament %d needs loading", 3);
  // Two distinct buttons, not one: with a single button a mutant that names the first
  // twice, or swaps the pair, produces output no assertion on that one button separates.
  hostui.prompt_do(PROMPT_USER_CONTINUE, built, F("PurgeMore"), F("Continue"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_begin Filament 3 needs loading") != std::string::npos,
    "a message assembled at runtime should arrive as written");

  const size_t first = at(sent, "prompt_button PurgeMore"),
               second = at(sent, "prompt_button Continue");
  TEST_ASSERT_TRUE_MESSAGE(first != std::string::npos && second != std::string::npos,
    "and both its buttons should be named");
  TEST_ASSERT_TRUE_MESSAGE(first < second, "in the order they were given");
  TEST_ASSERT_TRUE_MESSAGE(second < at(sent, "prompt_show"), "and before the show");

  // The runtime path is a separate overload with its own copy of the opening sequence, so
  // it has to be shown doing the same two things: closing the previous prompt, and
  // recording what it is asking. Deleting either survived every assertion on the text.
  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_end") < at(sent, "prompt_begin"),
    "a runtime-string prompt should close whatever was showing, like any other");
  TEST_ASSERT_EQUAL_MESSAGE(PROMPT_USER_CONTINUE, hostui.host_prompt_reason,
    "and record its reason, or the answer would be routed to the wrong question");
}

/**
 * The trailing character is how the firmware says *which tool*.
 *
 * `prompt_do(PROMPT_FILAMENT_RUNOUT, F("FilamentRunout T"), tool)` — the message is a
 * constant and the extruder number is appended as a single character, which is why the
 * parameter exists. A host that received "FilamentRunout T" with nothing after it would
 * tell the user a filament had run out without saying which one, on exactly the machines
 * where it matters.
 */
MARLIN_TEST(host_prompts, the_tool_number_is_appended_to_the_message_that_names_it) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_do(PROMPT_FILAMENT_RUNOUT, F("FilamentRunout T"), '2', F("Continue"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_begin FilamentRunout T2") != std::string::npos,
    "the tool number should follow the message it belongs to");
}

/**
 * The overload that appends a character keeps the buttons straight.
 *
 * `prompt_do` exists four times over, and the two that take a trailing character have their
 * own copy of the line that emits the buttons. Two *distinct* buttons are needed to see it:
 * with one, a mutant that names the first twice, or swaps the pair, or drops the line
 * entirely, produces output no assertion on a single button can separate. This is the
 * shape real callers use — `pause.cpp` asks "Load Filament T0" with a Continue button.
 */
MARLIN_TEST(host_prompts, a_prompt_with_a_tool_number_still_offers_both_its_buttons_in_order) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_do(PROMPT_USER_CONTINUE, F("Load Filament T"), '0', F("PurgeMore"), F("Continue"));
  const std::string sent = host.finish();

  const size_t first = at(sent, "prompt_button PurgeMore"),
               second = at(sent, "prompt_button Continue"),
               shown = at(sent, "prompt_show");

  TEST_ASSERT_TRUE_MESSAGE(first  != std::string::npos, "the first button should be named");
  TEST_ASSERT_TRUE_MESSAGE(second != std::string::npos, "and the second, not the first twice");
  TEST_ASSERT_TRUE_MESSAGE(first < second, "in the order they were given");
  TEST_ASSERT_TRUE_MESSAGE(second < shown, "and both before the show");
}

/**
 * The same again for a message built at runtime.
 *
 * `prompt_do` is four overloads: message from program memory or from RAM, each with and
 * without a trailing character. Each has its own copy of the two lines that open the prompt
 * and emit the buttons, so covering three of them leaves the fourth's copies unasserted —
 * which is what a duplicated body costs. Nothing in the firmware calls this one today; it
 * is public API and a caller that appended a tool number to a name it had assembled would
 * land here.
 */
MARLIN_TEST(host_prompts, a_runtime_message_with_a_tool_number_behaves_like_the_others) {
  QuietHost quiet;
  SerialCapture host;

  char built[32];
  snprintf(built, sizeof(built), "Runout T");
  hostui.prompt_do(PROMPT_FILAMENT_RUNOUT, built, '1', F("PurgeMore"), F("Continue"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_begin Runout T1") != std::string::npos,
    "the assembled message should arrive with its trailing character");

  const size_t first = at(sent, "prompt_button PurgeMore"),
               second = at(sent, "prompt_button Continue");
  TEST_ASSERT_TRUE_MESSAGE(first != std::string::npos && second != std::string::npos,
    "and both buttons should be named");
  TEST_ASSERT_TRUE_MESSAGE(first < second, "in the order they were given");
  TEST_ASSERT_TRUE_MESSAGE(second < at(sent, "prompt_show"), "and before the show");
}

// ...and a prompt given no trailing character gains none, which is the other arm of the
// same guard and the one every other test here takes.
MARLIN_TEST(host_prompts, a_message_with_no_trailing_character_gains_nothing) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_do(PROMPT_INFO, F("Plain"), F("Continue"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_begin Plain\n") != std::string::npos,
    "a message with nothing to append should end where it ends");
}

/**
 * A notification is not a prompt: it tells the host something and asks nothing.
 *
 * Worth its own test because the two share `action()` and differ only in the verb, so a
 * mistake here reads as a dialogue box the user cannot dismiss.
 */
MARLIN_TEST(host_prompts, a_notification_says_its_piece_and_asks_nothing) {
  QuietHost quiet;
  SerialCapture host;

  hostui.notify(F("Bed levelled"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "//action:notification Bed levelled") != std::string::npos,
    "a notification should reach the host as a notification");
  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt") == std::string::npos,
    "and must not be a prompt, which would wait for an answer that never comes");
}

/**
 * `prompt_open` is the one that defers to whatever is already asked.
 *
 * Everything else replaces the current prompt; this one is for a subsystem that wants to
 * ask *unless the machine is already asking something more important*. The difference is
 * invisible in a single call and is the whole reason the function exists.
 */
MARLIN_TEST(host_prompts, an_opening_prompt_gives_way_to_one_already_asked) {
  QuietHost quiet;

  {
    SerialCapture host;
    hostui.prompt_do(PROMPT_USER_CONTINUE, F("First question"), F("Continue"));
    host.finish();
  }

  SerialCapture host;
  hostui.prompt_open(PROMPT_INFO, F("Second question"), F("Continue"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "Second question") == std::string::npos,
    "a prompt raised while one is outstanding should not talk over it");
  TEST_ASSERT_EQUAL_MESSAGE(PROMPT_USER_CONTINUE, hostui.host_prompt_reason,
    "and must not take over the reason, or the answer would be routed to the wrong question");
}

MARLIN_TEST(host_prompts, an_opening_prompt_is_asked_when_nothing_is_outstanding) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_open(PROMPT_INFO, F("Only question"), F("Continue"));
  const std::string sent = host.finish();

  TEST_ASSERT_TRUE_MESSAGE(at(sent, "prompt_begin Only question") != std::string::npos,
    "with nothing outstanding the prompt should be asked normally");
}

/**
 * An answer is consumed, and consumed once.
 *
 * `handle_response()` clears the reason before it acts, so a host that sends `M876` twice —
 * or a stale one arriving late on a busy link — cannot answer a *later* question that
 * happens to be outstanding by then. The clearing is one line with no other observable
 * effect, and the reason field is the only thing that shows it happened.
 *
 * Asserted through `M876` rather than by calling `handle_response()` directly, because the
 * command is how a host actually answers and the wiring between them is part of the claim.
 */
MARLIN_TEST(host_prompts, answering_a_prompt_consumes_the_question) {
  QuietHost quiet;
  SerialCapture host;

  hostui.continue_prompt(F("Take the part off"));
  TEST_ASSERT_EQUAL_MESSAGE(PROMPT_USER_CONTINUE, hostui.host_prompt_reason,
    "the prompt should have recorded what it was asking");

  host_sends("M876 S1");
  host.finish();

  TEST_ASSERT_EQUAL_MESSAGE(PROMPT_NOT_DEFINED, hostui.host_prompt_reason,
    "answering should leave no question outstanding, so a repeat answers nothing");
}

// M876 with no S is not an answer at all, and must not be read as button zero.
MARLIN_TEST(host_prompts, M876_without_a_button_answers_nothing) {
  QuietHost quiet;
  SerialCapture host;

  hostui.continue_prompt(F("Take the part off"));
  host_sends("M876");
  host.finish();

  TEST_ASSERT_EQUAL_MESSAGE(PROMPT_USER_CONTINUE, hostui.host_prompt_reason,
    "an M876 with no button is not an answer and should leave the question outstanding");
}

#if HAS_RESUME_CONTINUE

/**
 * The reply is routed by the question, not by the button.
 *
 * `M876 S1` means "the user pressed the second button", and what that does depends on what
 * was asked. Against a wait it must release the machine; against a message the user was
 * only being shown, the identical `S1` must not. Nothing about the response value itself
 * distinguishes them — the routing is the behaviour.
 *
 * Only compiled where a wait exists to release. In the default configuration
 * `HAS_RESUME_CONTINUE` is off and that arm of `handle_response()` is preprocessed away, so
 * there is nothing here to assert rather than something going unasserted.
 */
MARLIN_TEST(host_prompts, answering_a_continue_prompt_releases_the_machine) {
  QuietHost quiet;
  SerialCapture host;

  hostui.continue_prompt(F("Take the part off"));
  marlin.wait_for_user = true;
  host_sends("M876 S1");
  host.finish();

  TEST_ASSERT_FALSE_MESSAGE(marlin.wait_for_user,
    "answering a continue prompt should release the machine from its wait");
}

MARLIN_TEST(host_prompts, the_same_answer_to_a_different_question_does_not_release_it) {
  QuietHost quiet;
  SerialCapture host;

  hostui.prompt_do(PROMPT_INFO, F("Just so you know"), F("Dismiss"));
  TEST_ASSERT_EQUAL(PROMPT_INFO, hostui.host_prompt_reason);

  marlin.wait_for_user = true;
  host_sends("M876 S1");
  host.finish();

  TEST_ASSERT_TRUE_MESSAGE(marlin.wait_for_user,
    "the same button against a message the user was only being shown must not resume a wait");

  marlin.wait_for_user = false;
}

// The consumed-once property, seen from the side where it has consequences.
MARLIN_TEST(host_prompts, a_repeated_answer_cannot_release_a_later_wait) {
  QuietHost quiet;
  SerialCapture host;

  hostui.continue_prompt(F("Take the part off"));
  marlin.wait_for_user = true;
  host_sends("M876 S1");
  TEST_ASSERT_FALSE(marlin.wait_for_user);

  marlin.wait_for_user = true;          // waiting again, for something unrelated
  host_sends("M876 S1");                // ... and the host's answer arrives late
  host.finish();

  TEST_ASSERT_TRUE_MESSAGE(marlin.wait_for_user,
    "a repeated answer should find no question outstanding and release nothing");

  marlin.wait_for_user = false;
}

#endif // HAS_RESUME_CONTINUE

#endif // HOST_PROMPT_SUPPORT
