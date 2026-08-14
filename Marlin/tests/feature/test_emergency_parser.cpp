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

#endif // EMERGENCY_PARSER
