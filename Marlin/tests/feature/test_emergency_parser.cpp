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
 * The stop that works when nothing else does.
 *
 * The emergency parser reads the serial stream *character by character as it arrives*, ahead of
 * the queue and ahead of any command being executed. That is the whole point: a machine wedged in
 * a two-hour print, a full command buffer, a heater wait that will not return — none of it can
 * stop `M112` being seen, because seeing it does not require the firmware to be doing anything
 * else. It is the last thing between a runaway printer and somebody reaching for the mains.
 *
 * `EmergencyParser::update(state, c)` is a pure state machine over a state and one byte: no
 * hardware, no clock, no fixture. So these tests feed it strings and look at the flags, which
 * makes them the cheapest and most exact tests in this suite — and it was at **0%**.
 *
 * Two properties matter and they pull against each other: it must recognise its handful of
 * commands in a stream of arbitrary G-code, and recognise as little else as possible. Writing the
 * second half is what found defect #59 — the parser does fire on `M1121`, and the test below that
 * was written to prove otherwise is the one that failed.
 */

#include "../test/unit_tests.h"
#include "src/inc/MarlinConfig.h"

#if ENABLED(EMERGENCY_PARSER)

#include "src/feature/e_parser.h"
#include "src/MarlinCore.h"

namespace {

  /**
   * Restores the flags, which are process-wide statics that other code acts on.
   *
   * `killed_by_M112` in particular is read by the command queue, which halts the machine when it
   * sees it set. A test that left it true would stop the *next* test's machine, and the failure
   * would appear to be about whatever that test was doing.
   */
  struct ParserFlags {
    bool was_killed, was_quickstop, was_enabled;
    #if HAS_MEDIA
      bool was_abort;
    #endif
    ParserFlags()
      : was_killed(EmergencyParser::killed_by_M112),
        was_quickstop(EmergencyParser::quickstop_by_M410),
        was_enabled(EmergencyParser::isEnabled())
        #if HAS_MEDIA
          , was_abort(EmergencyParser::sd_abort_by_M524)
        #endif
    {
      EmergencyParser::killed_by_M112 = false;
      EmergencyParser::quickstop_by_M410 = false;
      TERN_(HAS_MEDIA, EmergencyParser::sd_abort_by_M524 = false);
      EmergencyParser::enable();
    }
    ~ParserFlags() {
      EmergencyParser::killed_by_M112 = was_killed;
      EmergencyParser::quickstop_by_M410 = was_quickstop;
      TERN_(HAS_MEDIA, EmergencyParser::sd_abort_by_M524 = was_abort);
      if (was_enabled) EmergencyParser::enable(); else EmergencyParser::disable();
    }
  };

  // Feed a whole string through, starting from a fresh line, and report where it ended up.
  EmergencyParser::State feed(const char * const text,
                              EmergencyParser::State from = EmergencyParser::EP_RESET) {
    EmergencyParser::State state = from;
    for (const char *p = text; *p; ++p) EmergencyParser::update(state, uint8_t(*p));
    return state;
  }

}

/**
 * `M112` halts the machine, and only once the line is complete.
 *
 * The flag is raised on the *newline*, not on the final digit, and that is deliberate: until the
 * line ends the parser cannot know it is looking at `M112` rather than `M1120`. Asserting the
 * unterminated case is what pins that — without it, an implementation that fired on the last
 * character would pass, and would then halt a machine part-way through reading `M1121`.
 */
MARLIN_TEST(emergency_parser, M112_halts_the_machine_when_the_line_ends) {
  ParserFlags flags;

  feed("M112");
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112,
    "an unterminated M112 could still turn out to be M1120, and must not fire yet");

  feed("M112\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "a complete M112 line should halt the machine");
}

/**
 * ...and nothing that merely looks like it does.
 *
 * This is the half that makes the parser safe to run over every byte of every job. It sees
 * ordinary G-code all day — coordinates, checksums, comments, filenames — and a false positive
 * stops a print for no reason. Each of these is one edit away from the real thing.
 *
 * These are the ones that are genuinely rejected, and they are rejected early: each diverges
 * before the command number is complete, so the parser drops to `EP_IGNORE` and stays there until
 * the line ends. `*M112` stands for a checksum or any line whose first character is not a command
 * letter. The near-misses that are *not* rejected — a longer number with the same prefix — are
 * defect #59 and are pinned separately below.
 */
MARLIN_TEST(emergency_parser, nothing_that_merely_resembles_M112_halts_the_machine) {
  ParserFlags flags;

  for (const char * const near_miss : { "M11\n", "M113\n", "M12\n", "M212\n",
                                        "*M112\n", "G1 X112\n", "M11 2\n" }) {
    EmergencyParser::killed_by_M112 = false;
    feed(near_miss);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s should not halt the machine", near_miss);
    TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112, msg);
  }
}

/**
 * A line number in front of it makes no difference.
 *
 * Hosts number and checksum their lines, so the emergency commands arrive as `N4711 M112*23` far
 * more often than bare. The parser has a state for exactly this, and a version that only matched
 * from the start of the line would work perfectly in a terminal and fail against every real host.
 */
MARLIN_TEST(emergency_parser, a_numbered_line_still_halts_the_machine) {
  ParserFlags flags;

  feed("N4711 M112\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "a line-numbered M112 is what a host actually sends, and should halt the machine");
}

/**
 * Space between the letter and the number is tolerated.
 *
 * `M 112` is legal G-code and some senders produce it. The parser skips spaces in the states
 * where they can legitimately appear, which is worth pinning because it is invisible from the
 * command's name.
 */
MARLIN_TEST(emergency_parser, a_space_after_the_letter_is_tolerated) {
  ParserFlags flags;

  feed("M 112\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "M 112 is the same command and should halt the machine");
}

/**
 * The other emergency commands are recognised as themselves.
 *
 * They share every state up to the second character, so a fault that collapsed them would still
 * pass a test of `M112` alone — and would answer a stop request by aborting the print, or the
 * reverse. Each is asserted to fire *and* the others asserted not to, which is what separates
 * "recognised" from "recognised as the right one".
 */
MARLIN_TEST(emergency_parser, each_emergency_command_is_recognised_as_itself) {
  ParserFlags flags;

  feed("M410\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::quickstop_by_M410,
    "M410 should request a quickstop");
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112,
    "and should not halt the machine, which is a different and much larger action");

  #if HAS_MEDIA
    feed("M524\n");
    TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::sd_abort_by_M524,
      "M524 should request the print be abandoned");
    TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112,
      "and still should not halt the machine");
  #endif
}

/**
 * Disabling the parser stops it acting, and it recovers when re-enabled.
 *
 * `disable()` is used where the serial stream is carrying something that is not G-code — a binary
 * file transfer, where arbitrary bytes will sooner or later spell `M112`. The state machine keeps
 * running; only the actions are withheld. Both directions, because a parser that never came back
 * would leave the machine with no emergency stop for the rest of the session, and nothing would
 * report it.
 */
MARLIN_TEST(emergency_parser, a_disabled_parser_does_not_act_and_recovers_afterwards) {
  ParserFlags flags;

  EmergencyParser::disable();
  feed("M112\n");
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112,
    "a disabled parser should not act on M112 - the bytes may not be G-code at all");

  EmergencyParser::enable();
  feed("M112\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "and re-enabling it should restore the emergency stop");
}

/**
 * The parser recovers from a line it did not understand.
 *
 * Everything unrecognised goes to `EP_IGNORE` and stays there until end-of-line, which is what
 * lets the parser sit in a stream of arbitrary G-code. The claim worth making is the *recovery*:
 * an ignored line must not consume the line after it. If it did, the emergency stop would be
 * available only immediately after another emergency command.
 */
MARLIN_TEST(emergency_parser, an_unrecognised_line_does_not_swallow_the_next_one) {
  ParserFlags flags;

  feed("G1 X10 Y20 E5 F3000\nM112\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "an ordinary move followed by M112 should still halt the machine");
}

/**
 * LEGACY-BEHAVIOR: defect #59 — a longer command number that *starts* with an emergency command
 * triggers it. `M1121` halts the machine.
 *
 * Once the state machine reaches `EP_M112` nothing moves it on but end-of-line: the outer switch
 * has no case for the terminal states, so any further character falls to `default:`, which only
 * acts `if (ISEOL(c))` and otherwise leaves the state alone. That stickiness is *wanted* for
 * parameters and comments — `M112 ; stop now` has to work — and the same code cannot tell a
 * trailing digit from a trailing space.
 *
 * So it fires on `M1121`, and on `M4100`, `M1080` and `M5240` by the same route. None of those is
 * a real command, which is what keeps this latent: a host does not send them. A file might, and
 * the consequence is a print halted at a line that meant nothing.
 *
 * The correction is small — a digit arriving in a terminal state should fall to `EP_IGNORE` — but
 * it changes what a machine does with its emergency stop, so it is recorded rather than made.
 * Both halves are pinned: the trailing digit that should not fire and the trailing comment that
 * should, because a fix that stopped `M112 ; stop` working would be worse than the defect.
 */
MARLIN_TEST(emergency_parser, a_longer_command_number_starting_with_M112_also_halts) {
  ParserFlags flags;

  feed("M1121\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "M1121 halts the machine, because a terminal state absorbs anything up to end-of-line "
    "(defect #59)");

  EmergencyParser::killed_by_M112 = false;
  feed("M112 ; stop now\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "and the same stickiness is why a commented M112 still works, which is the half worth keeping");

  EmergencyParser::quickstop_by_M410 = false;
  feed("M4100\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::quickstop_by_M410,
    "the same shape on M410, so this is the state machine's rule rather than one bad arm");
}

/**
 * A carriage return ends a line as well as a newline.
 *
 * Hosts and files disagree about line endings — `\n`, `\r\n`, and bare `\r` all arrive in
 * practice — and the emergency stop is the last command that should care. `ISEOL()` accepts
 * either, and a version that only recognised `\n` would leave a whole class of senders with no
 * emergency stop at all, silently, while working perfectly on the developer's terminal.
 */
MARLIN_TEST(emergency_parser, a_carriage_return_ends_the_line_too) {
  ParserFlags flags;

  feed("M112\r");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "a bare carriage return should end the line and halt the machine");

  EmergencyParser::killed_by_M112 = false;
  feed("G1 X1\rM112\r\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "and an ignored line ended by a carriage return should not swallow the next one");
}

/**
 * A negative line number does not throw the parser off.
 *
 * `N-1` is what a host sends to reset the line numbering, and the parser accepts `-` inside the
 * number for exactly that reason. It is one character in a `case` list and invisible from
 * anywhere else — but a parser that dropped to `EP_IGNORE` on it would ignore the rest of that
 * line, and the line a host attaches to a numbering reset is often the one that matters.
 */
MARLIN_TEST(emergency_parser, a_negative_line_number_is_tolerated) {
  ParserFlags flags;

  feed("N-1 M112\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "a line-number reset followed by M112 should still halt the machine");
}

/**
 * A command that diverges from an emergency command before the end is rejected.
 *
 * `M411` shares every state with `M410` until its final character, which is where the state
 * machine must drop it. This is the case that separates "diverges early" from defect #59's
 * "diverges after the number is already complete" — the first is handled correctly and the second
 * is not, and having both pinned is what makes the register entry a statement about *where* the
 * boundary is rather than a vague complaint.
 */
MARLIN_TEST(emergency_parser, a_command_that_diverges_before_the_end_is_rejected) {
  ParserFlags flags;

  feed("M411\n");
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::quickstop_by_M410,
    "M411 diverges from M410 at the last character and should be rejected");

  feed("M410\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::quickstop_by_M410,
    "while M410 itself still works, so the rejection is selective rather than total");
}

/**
 * The parser lets go of a command once it has acted on it.
 *
 * The terminal states are sticky by design (defect #59), so the *reset* after acting is the only
 * thing that ends them. Without it the machine would stay in `EP_M410` for ever and fire a
 * quickstop at the end of **every subsequent line** — one emergency command would turn into an
 * unstoppable stutter that no later command could clear, and the printer would be unusable until
 * power-cycled.
 *
 * Asserted on `M410` rather than `M112`, because a halted machine cannot demonstrate a second
 * halt: `killed_by_M112` is already set and the assertion could not tell a repeat from the
 * original.
 */
MARLIN_TEST(emergency_parser, the_parser_lets_go_of_a_command_after_acting_on_it) {
  ParserFlags flags;

  // The state must be carried from one line to the next, because that is the whole claim: the
  // parser has to *arrive* at the second line having let go of the first. Starting the second
  // line from a fresh state asserts nothing — which is what the first draft did, and the mutant
  // that deletes the reset survived it.
  const EmergencyParser::State after_the_stop = feed("M410\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::quickstop_by_M410, "the quickstop should be requested");

  EmergencyParser::quickstop_by_M410 = false;
  feed("G1 X10 Y10\n", after_the_stop);
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::quickstop_by_M410,
    "an ordinary move after M410 must not request another quickstop - the parser has to have "
    "let go of the command it already acted on");
}

/**
 * Rubbish before the command does not become the command.
 *
 * Anything unrecognised sends the parser to `EP_IGNORE`, and it must stay there for the **whole**
 * line. A version that resynchronised on the next character would find `M112` inside any line
 * containing those characters — a filename, a comment, a checksum — and halt a print for it. The
 * two leading characters matter: with only one, a parser that reset immediately would consume the
 * `M` while resetting and still not fire, so the fault would hide.
 */
MARLIN_TEST(emergency_parser, rubbish_before_a_command_does_not_become_the_command) {
  ParserFlags flags;

  feed("XXM112\n");
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112,
    "M112 embedded in an unrecognised line must not halt the machine");

  feed("; M112 in a comment\n");
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112,
    "nor should a commented one");
}

/**
 * The action waits for end-of-line even when more characters arrive.
 *
 * A trailing character is what separates "fires when the number is complete" from "fires when the
 * line is complete", and only the second is correct — the parser cannot know whether it is looking
 * at `M112` or `M1120` until the line ends. This is the same claim the first test makes, one
 * character further on, and it is the character that distinguishes the two implementations.
 */
MARLIN_TEST(emergency_parser, a_trailing_character_does_not_trigger_the_action_early) {
  ParserFlags flags;

  // The state has to be carried between the two halves: `feed()` starts a fresh line unless it is
  // given somewhere to start from, and the first draft of this test began a new line for the
  // newline — which reset the parser and asserted nothing at all.
  const EmergencyParser::State part_way = feed("M112 ");
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112,
    "M112 followed by a space and no newline has not finished arriving");

  feed("\n", part_way);
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "and completing the line is what makes it act");
}

/**
 * A digit in the wrong place is not skipped over.
 *
 * `M3112` and `M1312` are the two ways a stray digit can land inside the command number. Both must
 * be abandoned outright — a parser that merely ignored the odd character and carried on matching
 * would find `M112` inside a great deal of ordinary G-code, and each of these lines corresponds to
 * one arm of the state machine giving up.
 */
MARLIN_TEST(emergency_parser, a_stray_digit_inside_the_command_number_abandons_the_line) {
  ParserFlags flags;

  for (const char * const line : { "M3112\n", "M1312\n", "M9 112\n" }) {
    EmergencyParser::killed_by_M112 = false;
    feed(line);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s should not halt the machine", line);
    TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::killed_by_M112, msg);
  }

  // ...and M41 followed by anything other than a zero is likewise abandoned, which is the same
  // rule one command along.
  feed("M41 \n");
  TEST_ASSERT_FALSE_MESSAGE(EmergencyParser::quickstop_by_M410,
    "M41 followed by a space is not M410 and should not request a quickstop");
}

/**
 * A line number containing a zero is still a line number.
 *
 * The digits are one `case` range, and a range is exactly the kind of thing that is written with
 * an off-by-one and never noticed: every host numbers lines from 1, so `N10` is the first line
 * where a missing `'0'` would bite — and it would bite by ignoring the rest of that line.
 */
MARLIN_TEST(emergency_parser, a_line_number_containing_a_zero_is_still_a_line_number) {
  ParserFlags flags;

  feed("N100 M112\n");
  TEST_ASSERT_TRUE_MESSAGE(EmergencyParser::killed_by_M112,
    "a line number with zeroes in it should not stop M112 being seen");
}

#endif // EMERGENCY_PARSER
