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
 * Tests for the command queue.
 *
 * The queue is what turns a line of text into something the printer does: commands are
 * buffered as they arrive and executed one at a time, and features inject their own
 * commands the same way. These tests put a command in and check that the setting it
 * names actually changed — the whole path from text to effect.
 */

#include "../test/unit_tests.h"
#include "src/gcode/queue.h"
#include "src/gcode/gcode.h"
#include "src/module/motion.h"
#include "src/module/planner.h"
#include "src/MarlinCore.h"
#include "../support/simulated_machine.h"
#include "serial_capture.h"
#include <string.h>
#include <stdio.h>

namespace {

  /**
   * An empty queue, and a serial port with nobody listening, for the whole test.
   *
   * Both matter: enqueueing echoes the command back to the host, and the native HAL's
   * serial write busy-waits for room in a 128-byte buffer that nothing drains here, so
   * a handful of enqueues would otherwise spin forever. See CLAUDE.md.
   */
  struct CleanQueue {
    int16_t was_feedrate;
    bool was_connected;
    CleanQueue() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      queue.clear();
      was_feedrate = motion.feedrate_percentage;
    }
    ~CleanQueue() {
      queue.clear();
      motion.feedrate_percentage = was_feedrate;
      MYSERIAL1.host_connected = was_connected;
    }
  };

  // Run the queue until it is empty. The guard stops a runaway test rather than
  // masking one: a queue that will not drain is a failure, not a slow success.
  void drain_queue() {
    for (uint8_t guard = 0; guard < 64 && queue.has_commands_queued(); guard++)
      queue.advance();
  }

}

MARLIN_TEST(queue, starts_empty) {
  CleanQueue clean;
  TEST_ASSERT_FALSE(queue.has_commands_queued());
  TEST_ASSERT_TRUE(queue.ring_buffer.empty());
  TEST_ASSERT_EQUAL(0, queue.ring_buffer.length);
}

MARLIN_TEST(queue, an_enqueued_command_is_held_then_run) {
  CleanQueue clean;

  TEST_ASSERT_TRUE(queue.enqueue_one("M220 S55"));
  TEST_ASSERT_TRUE(queue.has_commands_queued());
  TEST_ASSERT_EQUAL(1, queue.ring_buffer.length);

  // Still queued, so nothing has happened yet.
  TEST_ASSERT_NOT_EQUAL(55, motion.feedrate_percentage);

  drain_queue();
  TEST_ASSERT_EQUAL(55, motion.feedrate_percentage);
  TEST_ASSERT_FALSE(queue.has_commands_queued());
}

MARLIN_TEST(queue, commands_run_in_the_order_they_arrived) {
  CleanQueue clean;

  queue.enqueue_one("M220 S10");
  queue.enqueue_one("M220 S20");
  queue.enqueue_one("M220 S30");
  TEST_ASSERT_EQUAL(3, queue.ring_buffer.length);

  drain_queue();
  TEST_ASSERT_EQUAL(30, motion.feedrate_percentage);   // the last one wins
}

MARLIN_TEST(queue, the_buffer_fills_and_refuses_more) {
  CleanQueue clean;

  uint8_t accepted = 0;
  for (uint8_t i = 0; i < BUFSIZE + 4; i++)
    if (queue.enqueue_one("M220 S100")) accepted++;

  TEST_ASSERT_EQUAL(BUFSIZE, accepted);
  TEST_ASSERT_TRUE(queue.ring_buffer.full());
  TEST_ASSERT_EQUAL(BUFSIZE, queue.ring_buffer.length);

  drain_queue();
  TEST_ASSERT_TRUE(queue.ring_buffer.empty());
}

// The ring buffer wraps: filling, draining and filling again must keep working.
MARLIN_TEST(queue, the_ring_buffer_wraps_around) {
  CleanQueue clean;

  for (uint8_t round = 0; round < 3; round++) {
    for (uint8_t i = 0; i < BUFSIZE; i++)
      TEST_ASSERT_TRUE(queue.enqueue_one("M220 S100"));
    TEST_ASSERT_TRUE(queue.ring_buffer.full());
    drain_queue();
    TEST_ASSERT_TRUE(queue.ring_buffer.empty());
  }

  queue.enqueue_one("M220 S77");
  drain_queue();
  TEST_ASSERT_EQUAL(77, motion.feedrate_percentage);
}

MARLIN_TEST(queue, clear_discards_everything_waiting) {
  CleanQueue clean;

  queue.enqueue_one("M220 S10");
  queue.enqueue_one("M220 S20");
  TEST_ASSERT_TRUE(queue.has_commands_queued());

  queue.clear();
  TEST_ASSERT_FALSE(queue.has_commands_queued());
  TEST_ASSERT_EQUAL(0, queue.ring_buffer.length);

  drain_queue();
  TEST_ASSERT_NOT_EQUAL(20, motion.feedrate_percentage);   // never ran
}

// Injected commands are how features run G-code of their own.
MARLIN_TEST(queue, an_injected_command_runs) {
  CleanQueue clean;

  queue.inject(F("M220 S65"));
  drain_queue();
  TEST_ASSERT_EQUAL(65, motion.feedrate_percentage);
}

// Injection takes priority: it runs before whatever is already waiting.
MARLIN_TEST(queue, an_injected_command_runs_before_the_queue) {
  CleanQueue clean;

  queue.enqueue_one("M220 S10");
  queue.inject(F("M220 S90"));

  queue.advance();                 // one step: the injected command
  TEST_ASSERT_EQUAL(90, motion.feedrate_percentage);

  drain_queue();                   // then the queued one
  TEST_ASSERT_EQUAL(10, motion.feedrate_percentage);
}

// Several commands can be injected at once, separated by newlines.
MARLIN_TEST(queue, injected_commands_can_be_a_list) {
  CleanQueue clean;

  queue.inject(F("M220 S30\nM220 S40"));
  drain_queue();
  TEST_ASSERT_EQUAL(40, motion.feedrate_percentage);
}

/**
 * enqueue_one() on a blank line.
 *
 * A truly empty string and a line that is only an EOL character are both "nothing to
 * do", but for different reasons in the guard `*cmd == 0 || ISEOL(*cmd)` — and the two
 * clauses have to be checked separately, or a mutant that drops one of them survives on
 * whichever input the other clause still catches. A byte with the top bit set is thrown
 * in because `char` is signed here: a relational mutant of `== 0` (`<= 0`, `< 0`) agrees
 * with the true guard on every ordinary command but disagrees on a negative byte, which
 * is neither empty nor an end-of-line character and must still be queued.
 */
MARLIN_TEST(queue, enqueue_one_of_an_empty_string_does_not_queue_it) {
  CleanQueue clean;
  TEST_ASSERT_TRUE(queue.enqueue_one(""));
  TEST_ASSERT_EQUAL(0, queue.ring_buffer.length);
}

MARLIN_TEST(queue, enqueue_one_of_a_bare_newline_does_not_queue_it) {
  CleanQueue clean;
  TEST_ASSERT_TRUE(queue.enqueue_one("\n"));
  TEST_ASSERT_EQUAL(0, queue.ring_buffer.length);
}

MARLIN_TEST(queue, enqueue_one_of_a_high_bit_byte_is_not_mistaken_for_blank) {
  CleanQueue clean;
  const char cmd[] = { char(0x91), '\0' };
  TEST_ASSERT_TRUE(queue.enqueue_one(cmd));
  TEST_ASSERT_EQUAL(1, queue.ring_buffer.length);
}

/**
 * `RingBuffer::enqueue()` refuses two things directly: a comment line, and a full
 * buffer. Both have to be checked with their own input, or a mutant that folds one
 * clause to `false` survives on whatever the other clause still refuses.
 */
MARLIN_TEST(queue, a_comment_passed_directly_is_refused) {
  CleanQueue clean;
  TEST_ASSERT_FALSE(queue.enqueue_one(";not a command"));
  TEST_ASSERT_EQUAL(0, queue.ring_buffer.length);
}

MARLIN_TEST(queue, a_command_starting_below_the_comment_character_is_still_queued) {
  CleanQueue clean;
  // '0' (0x30) sits below ';' (0x3B): a relational mutant of *cmd == ';' would
  // wrongly refuse it, where only an actual ';' should be refused.
  TEST_ASSERT_TRUE(queue.enqueue_one("0test"));
  TEST_ASSERT_EQUAL(1, queue.ring_buffer.length);
}

/**
 * `inject(FSTR_P)` drains through `injected_commands_P`, from PROGMEM. There is a
 * second overload, `inject(const char*)`, that drains through `injected_commands` in
 * SRAM instead — a separate buffer with its own "is there anything queued" check
 * (`injected_commands[0] == '\0'`). Every other test in this file uses the PROGMEM
 * overload, so the SRAM path and its emptiness check have never run at all.
 */
MARLIN_TEST(queue, an_injected_sram_command_runs) {
  CleanQueue clean;
  char cmd[] = "M220 S65";
  queue.inject(cmd);
  queue.advance();                      // one step: process_injected_command() drains it
  TEST_ASSERT_EQUAL(65, motion.feedrate_percentage);
}

// advance() tries the SRAM injection queue before the ring buffer. With nothing
// injected, a genuinely queued command must still get its turn — which fails if the
// emptiness check ever answers "there is something" when there is not.
MARLIN_TEST(queue, an_empty_injected_sram_buffer_lets_the_ring_buffer_run) {
  CleanQueue clean;
  queue.injected_commands[0] = '\0';
  queue.injected_commands[1] = '\0';
  queue.enqueue_one("M220 S80");
  queue.advance();
  TEST_ASSERT_EQUAL(80, motion.feedrate_percentage);
}

// A list of SRAM-injected commands is drained one line at a time, the same as the
// PROGMEM path already covered by injected_commands_can_be_a_list — this exercises the
// copy loop that shifts the remainder down after the first line is consumed.
MARLIN_TEST(queue, injected_sram_commands_can_be_a_list) {
  CleanQueue clean;
  char cmd[] = "M220 S30\nM220 S40";
  queue.inject(cmd);
  queue.advance();
  TEST_ASSERT_EQUAL_MESSAGE(30, motion.feedrate_percentage, "the first line should run first");
  queue.advance();
  TEST_ASSERT_EQUAL_MESSAGE(40, motion.feedrate_percentage, "the second line should run after the shift");
}

// The emptiness check reads only injected_commands[0]. A mutant that reads index 1
// instead would see stale data left behind at that offset and wrongly claim there is
// something to run, which starves the ring buffer of its turn.
MARLIN_TEST(queue, process_injected_sram_command_reads_only_the_first_byte) {
  CleanQueue clean;
  char cmd[] = "AB";
  queue.inject(cmd);                    // injected_commands = "AB\0..."
  queue.injected_commands[0] = '\0';    // mark it empty again, leaving 'B' behind at [1]
  queue.enqueue_one("M220 S80");
  queue.advance();
  TEST_ASSERT_EQUAL_MESSAGE(80, motion.feedrate_percentage,
    "the emptiness check must read injected_commands[0], not a stale byte elsewhere");
}

MARLIN_TEST(queue, the_line_number_is_remembered_per_port) {
  CleanQueue clean;
  queue.set_current_line_number(123);
  TEST_ASSERT_EQUAL(123, queue.get_current_line_number());
  queue.set_current_line_number(0);
  TEST_ASSERT_EQUAL(0, queue.get_current_line_number());
}

/**
 * Receiving commands over the serial port.
 *
 * A host does not call the queue's API — it sends bytes. Those bytes carry a line
 * number and a checksum so that a dropped or corrupted line is caught rather than
 * printed, and this is the code that decides. Tests below put bytes into the receive
 * buffer exactly as a host would and then let the queue read them.
 */
namespace {

  void host_transmits(const char * const line) {
    for (const char *p = line; *p; p++) MYSERIAL1.receive_buffer.write(uint8_t(*p));
    MYSERIAL1.receive_buffer.write(uint8_t('\n'));
    queue.get_available_commands();
  }

  // The checksum a host must append: XOR of every byte before the '*'.
  uint8_t checksum_of(const char * const line) {
    uint8_t sum = 0;
    for (const char *p = line; *p; p++) sum ^= uint8_t(*p);
    return sum;
  }

  // Send a line with a correct checksum, as a well-behaved host does.
  void host_transmits_checksummed(const char * const line) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s*%u", line, unsigned(checksum_of(line)));
    host_transmits(buf);
  }

}

/**
 * Everything up to the last character that fits is kept.
 *
 * The reader fills a `MAX_CMD_SIZE` buffer and sets `PS_EOL` once it holds
 * `MAX_CMD_SIZE - 1` characters, which skips the rest of the line. This asserts the cut
 * falls where it says it does, by putting a parameter so that its last character is the
 * last one the reader keeps: any reader that stops earlier loses it.
 *
 * **The far side of the cut is deliberately not asserted, and that is the finding.** The
 * obvious companion — the same parameter one character further along, expected to be lost
 * — passes just as well with the guard deleted, which was checked rather than assumed.
 * Without it the reader does not read *more* of the line, it writes past the end of the
 * buffer; the parameter is lost either way and for a different reason. So every mutant that
 * widens this guard is undefined behaviour rather than different behaviour, and no assertion
 * can separate them. Recorded in the register instead.
 *
 * Written after register #25, which left this branch unasserted because the test that
 * reached it hung. It no longer does; see the register entry.
 */
MARLIN_TEST(queue, a_parameter_on_the_last_character_that_fits_is_still_read) {
  CleanQueue clean;

  char line[MAX_CMD_SIZE];
  memset(line, ' ', sizeof(line));
  memcpy(line, "M220", 4);
  memcpy(line + (MAX_CMD_SIZE - 1) - 3, "S99", 3);
  line[MAX_CMD_SIZE - 1] = '\0';

  motion.feedrate_percentage = 50;
  host_transmits(line);
  drain_queue();
  TEST_ASSERT_EQUAL_MESSAGE(99, motion.feedrate_percentage,
    "a parameter ending on the last character the reader keeps should still be read");
}

/**
 * An over-long line is executed as far as it fits, not refused.
 *
 * Truncation is not rejection: the command at the head of the line still runs. A reader that
 * dropped the whole line on overflow would satisfy the test above and fail this one.
 *
 * The length is bounded by the *port*, not by the command buffer: a test writes into a
 * 128-byte receive buffer and anything past that is dropped on the floor — including the
 * newline, so the line never completes and no command appears at all. It has to be longer
 * than `MAX_CMD_SIZE` and shorter than the port, which is a narrow window and worth knowing
 * before writing another test like this one.
 */
MARLIN_TEST(queue, an_over_long_line_still_runs_the_command_at_its_head) {
  CleanQueue clean;

  char line[MAX_CMD_SIZE + 20];
  memset(line, 'x', sizeof(line));
  memcpy(line, "M220 S99 ", 9);
  line[sizeof(line) - 1] = '\0';

  motion.feedrate_percentage = 50;
  host_transmits(line);
  TEST_ASSERT_EQUAL_MESSAGE(1, queue.ring_buffer.length,
    "one over-long line should still be one command, not none and not two");

  drain_queue();
  TEST_ASSERT_EQUAL_MESSAGE(99, motion.feedrate_percentage,
    "and the command at the head of it should have run");
}

MARLIN_TEST(queue, a_transmitted_command_is_received_and_run) {
  CleanQueue clean;

  host_transmits("M220 S45");
  TEST_ASSERT_TRUE(queue.has_commands_queued());

  drain_queue();
  TEST_ASSERT_EQUAL(45, motion.feedrate_percentage);
}

MARLIN_TEST(queue, numbered_lines_are_accepted_in_sequence) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  host_transmits_checksummed("N1 M220 S11");
  host_transmits_checksummed("N2 M220 S22");
  drain_queue();

  TEST_ASSERT_EQUAL(22, motion.feedrate_percentage);
  TEST_ASSERT_EQUAL(2, queue.get_current_line_number());
}

// A line arriving out of order means one was lost, so it is refused rather than run.
MARLIN_TEST(queue, a_line_number_out_of_sequence_is_refused) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  host_transmits_checksummed("N1 M220 S11");
  drain_queue();
  TEST_ASSERT_EQUAL(11, motion.feedrate_percentage);

  host_transmits_checksummed("N9 M220 S99");   // 9 is not 2: a line went missing
  drain_queue();
  TEST_ASSERT_EQUAL(11, motion.feedrate_percentage);   // not run
}

// A line repeated because the host missed the acknowledgement is dropped quietly
// rather than run twice.
MARLIN_TEST(queue, a_repeated_line_number_is_ignored) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  host_transmits_checksummed("N1 M220 S11");
  drain_queue();

  motion.feedrate_percentage = 50;
  host_transmits_checksummed("N1 M220 S11");   // the same line again
  drain_queue();
  TEST_ASSERT_EQUAL(50, motion.feedrate_percentage);   // was not run a second time
}

// A corrupted line must not be printed.
MARLIN_TEST(queue, a_bad_checksum_is_refused) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  host_transmits("N1 M220 S88*1");             // 1 is not the real checksum
  drain_queue();
  TEST_ASSERT_NOT_EQUAL(88, motion.feedrate_percentage);
}

MARLIN_TEST(queue, a_correct_checksum_is_accepted) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  host_transmits_checksummed("N1 M220 S66");
  drain_queue();
  TEST_ASSERT_EQUAL(66, motion.feedrate_percentage);
}

// M110 is how a host resets the count, so it is exempt from the sequence check.
MARLIN_TEST(queue, M110_sets_the_line_number_out_of_sequence) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  host_transmits_checksummed("N1 M220 S11");
  drain_queue();

  host_transmits_checksummed("N500 M110 N500");
  drain_queue();
  TEST_ASSERT_EQUAL(500, queue.get_current_line_number());

  host_transmits_checksummed("N501 M220 S33");
  drain_queue();
  TEST_ASSERT_EQUAL(33, motion.feedrate_percentage);
}

MARLIN_TEST(queue, an_empty_line_is_harmless) {
  CleanQueue clean;
  host_transmits("");
  drain_queue();
  TEST_ASSERT_FALSE(queue.has_commands_queued());
}

// A one-character line is not empty: it must be queued, unlike a truly empty one.
MARLIN_TEST(queue, a_one_character_line_is_not_treated_as_empty) {
  CleanQueue clean;
  host_transmits("X");
  TEST_ASSERT_TRUE(queue.has_commands_queued());
  drain_queue();
}

// A comment line carries no command.
MARLIN_TEST(queue, a_comment_line_is_not_queued) {
  CleanQueue clean;
  const int16_t before = motion.feedrate_percentage;
  host_transmits("; M220 S5");
  drain_queue();
  TEST_ASSERT_EQUAL(before, motion.feedrate_percentage);
}

/**
 * A backslash escapes the character after it: the backslash itself is dropped, and
 * whatever follows is taken literally rather than being interpreted (as a comment
 * marker, a quote, or another backslash). The escape state has to return to normal
 * after each one or a second backslash later in the same line would be handled wrong,
 * which is what the second backslash here is for — it also stands in for the case
 * that only shows up once the state has been used and released before.
 */
MARLIN_TEST(queue, a_backslash_escapes_the_character_that_follows) {
  CleanQueue clean;
  host_transmits("M220 S\\5\\5");
  drain_queue();
  TEST_ASSERT_EQUAL(55, motion.feedrate_percentage);
}

/**
 * The backspace check is exact: only byte 0x08 erases. Bracket it on both sides — a
 * byte below it (0x01, an arbitrary control code) must be taken literally rather than
 * erasing anything, and the real backspace (0x08) must still erase.
 */
MARLIN_TEST(queue, backspace_erases_the_character_before_it) {
  CleanQueue clean;
  // Adjacent string literals, not "\x082": a hex escape consumes every hex digit that
  // follows it, so "\x082" is one character (0x82), not 0x08 then '2'.
  host_transmits("M220 S1" "\x08" "2");   // '1' typed, then erased, then '2': the fixed intent is 2
  drain_queue();
  TEST_ASSERT_EQUAL(2, motion.feedrate_percentage);
}

MARLIN_TEST(queue, a_control_character_below_backspace_is_not_taken_for_one) {
  CleanQueue clean;
  host_transmits("M220 S1" "\x01" "2");   // 0x01 is not backspace and must not erase the '1'
  drain_queue();
  TEST_ASSERT_EQUAL(1, motion.feedrate_percentage);
}

/**
 * Leading spaces are skipped before the line-number and emergency-command checks look
 * at fixed character positions — skip too few or too many and those checks look at
 * the wrong byte. A tab is not a space, so it must be left where it is rather than
 * being swept up by a mutant that skips "whitespace" more broadly.
 */
MARLIN_TEST(queue, leading_spaces_are_skipped_before_reading_an_emergency_command) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  marlin.wait_for_heatup = true;
  host_transmits("   M108");

  TEST_ASSERT_FALSE_MESSAGE(marlin.wait_for_heatup, "M108 should still be recognised after leading spaces");
}

MARLIN_TEST(queue, a_leading_tab_is_not_skipped_like_a_space) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  marlin.wait_for_heatup = true;
  host_transmits("\tM108");

  TEST_ASSERT_TRUE_MESSAGE(marlin.wait_for_heatup, "a tab is not a space and must not be swept up");

  marlin.wait_for_heatup = false;
  drain_queue();
}

/**
 * The resend window is two line numbers wide, and it has an edge.
 *
 * When the machine asks for a resend, the line it is complaining about may already be in
 * flight — so it arrives again, out of sequence, through no fault of the host. Repeating the
 * last line or the one before it is therefore ignored in silence; anything older than that is
 * a genuinely lost line and must be refused loudly, or the print continues with a gap in it.
 *
 * The three cases here sit on both sides of that edge, which is what pins the width of the
 * window rather than merely its existence.
 */
MARLIN_TEST(queue, a_line_number_just_behind_is_ignored_and_an_older_one_is_an_error) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  host_transmits_checksummed("N1 M220 S11");
  host_transmits_checksummed("N2 M220 S22");
  host_transmits_checksummed("N3 M220 S33");
  drain_queue();
  TEST_ASSERT_EQUAL(33, motion.feedrate_percentage);

  // N3 again — the last line, inside the window.
  motion.feedrate_percentage = 50;
  host_transmits_checksummed("N3 M220 S77");
  drain_queue();
  TEST_ASSERT_EQUAL_MESSAGE(50, motion.feedrate_percentage, "the last line repeated should be ignored");

  // N2 — one behind, still inside the window.
  host_transmits_checksummed("N2 M220 S78");
  drain_queue();
  TEST_ASSERT_EQUAL_MESSAGE(50, motion.feedrate_percentage, "the line before last should be ignored");

  // N1 — outside it. Refused, and the count is not disturbed by the refusal.
  host_transmits_checksummed("N1 M220 S79");
  drain_queue();
  TEST_ASSERT_EQUAL_MESSAGE(50, motion.feedrate_percentage, "an older line should be refused");

  // The sequence is still where it was, so the next line in order runs.
  host_transmits_checksummed("N4 M220 S44");
  drain_queue();
  TEST_ASSERT_EQUAL_MESSAGE(44, motion.feedrate_percentage, "a refusal should not move the sequence");
}

/**
 * The test above cannot tell "ignored" from "refused" apart, because neither ever runs
 * the repeated command: both leave the feedrate exactly where it was. What actually
 * differs is what goes back to the host — a repeat inside the window is silent, and a
 * genuinely lost line gets a resend request — which is the channel this asserts on
 * instead. See CLAUDE.md on negative assertions and on asserting the channel a message
 * came out on.
 */
MARLIN_TEST(queue, a_repeat_inside_the_window_asks_for_nothing_but_an_older_line_asks_for_a_resend) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  host_transmits_checksummed("N1 M220 S11");
  host_transmits_checksummed("N2 M220 S22");
  host_transmits_checksummed("N3 M220 S33");
  drain_queue();

  {
    SerialCapture capture;
    host_transmits_checksummed("N3 M220 S99");   // the last line, inside the window
    TEST_ASSERT_FALSE_MESSAGE(capture.saw("Resend:"), "a repeat of the last line must not ask for a resend");
  }
  {
    SerialCapture capture;
    host_transmits_checksummed("N2 M220 S99");   // one behind, still inside the window
    TEST_ASSERT_FALSE_MESSAGE(capture.saw("Resend:"), "a repeat of the line before last must not ask for a resend");
  }
  {
    SerialCapture capture;
    host_transmits_checksummed("N1 M220 S99");   // outside the window: genuinely lost
    TEST_ASSERT_TRUE_MESSAGE(capture.saw("Resend:"), "a line outside the window must ask for a resend");
  }

  drain_queue();
}

// The number named in "Resend: N" is last_N + 1 exactly — the line the host should
// send next. Pin the number, not just that a resend happened.
MARLIN_TEST(queue, a_resend_request_names_the_next_expected_line) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  SerialCapture capture;
  host_transmits("N1 M220 S11*1");   // wrong checksum: refused, resend requested
  TEST_ASSERT_TRUE_MESSAGE(capture.saw("Resend: 1"), "with last_N at 0, the next expected line is 1");
}

/**
 * Commands that cannot wait are acted on as the line is read.
 *
 * `M108` and `M410` exist to reach a machine that is *already* stuck — waiting for a
 * temperature, or part-way through a move queue that will take a minute to drain. Queueing
 * them would defeat them, so they are recognised in the serial reader before the line is
 * enqueued at all. That is the property to assert: the effect must be visible without the
 * queue being advanced even once.
 *
 * `M112` sits in the same switch and cannot be tested here — it calls `kill()`, which does not
 * return. See the defect register for the class.
 */
MARLIN_TEST(queue, M108_stops_a_wait_without_the_queue_being_advanced) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  marlin.wait_for_heatup = true;
  host_transmits("M108");                      // read only — drain_queue() is deliberately not called

  TEST_ASSERT_FALSE_MESSAGE(marlin.wait_for_heatup, "M108 should end the wait as it is read");
}

/**
 * A near-miss must not be taken for the emergency command.
 *
 * The switch tests three characters by position, so `M118` differs from `M108` in exactly the
 * one the switch does not select on. Without this the test above is satisfied by an
 * implementation that fires on any `M1x8`, which would abandon a heat-up whenever a host sent
 * a message to the display.
 */
MARLIN_TEST(queue, a_command_that_merely_looks_like_M108_does_not_stop_a_wait) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  marlin.wait_for_heatup = true;
  host_transmits("M118 hello");

  TEST_ASSERT_TRUE_MESSAGE(marlin.wait_for_heatup, "M118 should not be taken for M108");

  marlin.wait_for_heatup = false;              // leave nothing waiting behind
  drain_queue();
}

/**
 * The read-time switch only enters for an M-code (`command[0] == 'M'`). A relational
 * mutant of that check (`<= 'M'`, `>= 'M'`) is not visible on any M-command, because
 * 'M' compares equal to itself either way — it needs a command on the other side of
 * 'M' whose remaining characters would otherwise match the M108 case by position.
 */
MARLIN_TEST(queue, a_G_command_shaped_like_M108_does_not_stop_a_wait) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  marlin.wait_for_heatup = true;
  host_transmits("G108");                      // 'G' < 'M': command[0] <= 'M' would wrongly enter

  TEST_ASSERT_TRUE_MESSAGE(marlin.wait_for_heatup, "a G-command must not be read as M108");

  marlin.wait_for_heatup = false;
  drain_queue();
}

MARLIN_TEST(queue, a_T_command_shaped_like_M108_does_not_stop_a_wait) {
  CleanQueue clean;
  queue.set_current_line_number(0);

  marlin.wait_for_heatup = true;
  host_transmits("T108");                      // 'T' > 'M': command[0] >= 'M' would wrongly enter

  TEST_ASSERT_TRUE_MESSAGE(marlin.wait_for_heatup, "a T-command must not be read as M108");

  marlin.wait_for_heatup = false;
  drain_queue();
}

/**
 * `M410` is picked out by testing command[1] and command[2] against '4' and '1'
 * individually. Each comparison needs a value on both sides of the character it
 * checks, not just "some other command", or a relational mutant (`<=`, `>=`) that
 * still agrees on that one probe survives.
 */

MARLIN_TEST(queue, characters_either_side_of_M410_do_not_stop_the_steppers) {
  CleanQueue clean;
  SimulatedMachine machine;
  queue.set_current_line_number(0);

  xyze_pos_t target = { 0 };
  motion.position = target;
  planner.set_position_mm(target);
  for (uint8_t i = 1; i <= 3; ++i) {
    target.x = float(i);
    planner.buffer_line(target, 5.0f);
  }
  const uint8_t planned = planner.movesplanned();
  TEST_ASSERT_TRUE_MESSAGE(planned > 0, "the fixture planned no moves to leave alone");

  // command[1]: '5' > '4' and '3' < '4'
  host_transmits("M510");
  TEST_ASSERT_EQUAL_MESSAGE(planned, planner.movesplanned(), "M510 should not be taken for M410");
  host_transmits("M310");
  TEST_ASSERT_EQUAL_MESSAGE(planned, planner.movesplanned(), "M310 should not be taken for M410");

  // command[2]: '9' > '1' and '0' < '1'
  host_transmits("M490");
  TEST_ASSERT_EQUAL_MESSAGE(planned, planner.movesplanned(), "M490 should not be taken for M410");
  host_transmits("M400");
  TEST_ASSERT_EQUAL_MESSAGE(planned, planner.movesplanned(), "M400 should not be taken for M410");
}


/**
 * `M410` empties the planner where it stands.
 *
 * An emergency stop throws away motion that was planned but not yet performed. Asserting that
 * the planner is empty afterwards is the relationship — and the fixture has to prove there was
 * something there to throw away, or the assertion holds trivially.
 *
 * Test-HAL only, and `SimulatedMachine` rather than a local fixture, for two reasons that both
 * bite. `quick_stop()` sets a counter that only the temperature interrupt clears, and
 * `quickstop_stepper()` then calls `synchronize()`, which spins on `idle()` until it does — so
 * the command only returns under a HAL where time advances and interrupts fire. And a long
 * `idle()` wait is exactly what walks into the kill button that reads as held in a test build;
 * without the fixture's `release_kill_button()` this hangs after 250 passes, in some
 * configurations and not others, which is how it first showed up.
 */
MARLIN_TEST(queue, M410_discards_motion_that_had_been_planned_but_not_performed) {
  CleanQueue clean;
  SimulatedMachine machine;
  queue.set_current_line_number(0);

  xyze_pos_t target = { 0 };
  motion.position = target;
  planner.set_position_mm(target);
  for (uint8_t i = 1; i <= 3; ++i) {
    target.x = float(i);
    TEST_ASSERT_TRUE_MESSAGE(planner.buffer_line(target, 5.0f), "the fixture failed to plan a move");
  }
  TEST_ASSERT_TRUE_MESSAGE(planner.movesplanned() > 0,
    "the fixture planned nothing, so there is nothing for M410 to discard");

  host_transmits("M410");                      // recognised as the line is read

  TEST_ASSERT_EQUAL_MESSAGE(0, planner.movesplanned(), "M410 should empty the planner");
}

/**
 * A command one character away from `M410` leaves the motion alone.
 *
 * The switch selects on the fourth character and then checks the second and third, so `M411`
 * differs in the selector and `M100` differs in the pair. Neither is a real command in this
 * build, which does not matter: the emergency check runs on the raw line before anything is
 * parsed, so what is being tested is exactly the character comparison. Without this the test
 * above is satisfied by an implementation that stops on any `M4xx`.
 */
MARLIN_TEST(queue, a_command_one_character_away_from_M410_does_not_stop_the_steppers) {
  CleanQueue clean;
  SimulatedMachine machine;
  queue.set_current_line_number(0);

  xyze_pos_t target = { 0 };
  motion.position = target;
  planner.set_position_mm(target);
  for (uint8_t i = 1; i <= 3; ++i) {
    target.x = float(i);
    planner.buffer_line(target, 5.0f);
  }
  const uint8_t planned = planner.movesplanned();
  TEST_ASSERT_TRUE_MESSAGE(planned > 0, "the fixture planned no moves to leave alone");

  host_transmits("M411");
  TEST_ASSERT_EQUAL_MESSAGE(planned, planner.movesplanned(), "M411 should not be taken for M410");

  host_transmits("M100");
  TEST_ASSERT_EQUAL_MESSAGE(planned, planner.movesplanned(), "M100 should not be taken for M410");
}

