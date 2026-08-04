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
 * What the queue does while reading a print file, as opposed to running it.
 *
 * The existing media tests drive whole commands through the parser; these are about the
 * character-by-character read that happens before any command exists. Two things are decided
 * there and nowhere else, and mutation found both of them uncovered.
 *
 * The larger is a pause. A `M25` line *in the file* stops the fetch at the moment it is read,
 * before it is queued and long before it runs — because a pause that waited its turn in the
 * queue would let everything already buffered print first. That distinction is the whole
 * point of the branch, and it is only visible to a test that looks at the printing state
 * between the fetch and the run.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "../test/unit_tests.h"
#include "../support/simulated_media.h"

#include "src/sd/cardreader.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/gcode/queue.h"

#include <string.h>

namespace {

  struct StreamSlate {
    bool was_connected;
    StreamSlate() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      tidy();
    }
    ~StreamSlate() { tidy(); MYSERIAL1.host_connected = was_connected; }
    static void tidy() {
      card.flag.sdprinting = false;
      card.endFilePrintNow();
      if (card.isFileOpen()) card.closefile();
      if (!card.isMounted()) card.mount();
      card.cdroot();
      queue.clear();
    }
  };

  void send(const char * const line) {
    char buf[96];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  // A print running from the card: the file selected and started the way a host does it.
  void print_file(const char * const name, const char * const contents) {
    card.openFileWrite(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not write the print file");
    card.write((void*)contents, strlen(contents));
    card.closefile();

    char select[64];
    snprintf(select, sizeof(select), "M23 %s", name);
    send(select);
    send("M24");
    TEST_ASSERT_TRUE_MESSAGE(card.flag.sdprinting, "the print did not start");
  }

}

/**
 * `M25` in the file pauses the print as it is read.
 *
 * Fetching alone — no command is run — must be enough to stop the fetch. Asserting the state
 * *between* `get_available_commands()` and any `advance()` is what separates a pause applied
 * at read time from one applied when the command reaches the front of the queue; both would
 * end up paused, and only one of them stops the lines already buffered behind it from
 * printing first.
 */
MARLIN_TEST(queue_sd_stream, an_M25_in_the_file_pauses_the_print_as_it_is_read) {
  StreamSlate slate;

  print_file("PAUSING.GCO", "G1 X1\nM25\nG1 X2\n");

  queue.get_available_commands();     // read only — nothing is executed

  TEST_ASSERT_FALSE_MESSAGE(card.flag.sdprinting, "M25 in the file should pause the fetch");
}

/**
 * `M250` is not `M25`, and the fourth character is what says so.
 *
 * Same file, same position, one digit more — and the print must keep going. Without this the
 * test above is satisfied by any implementation that matches a three-character prefix, which
 * would pause on `M250`, `M251` and `M2500` alike.
 */
MARLIN_TEST(queue_sd_stream, a_longer_command_beginning_M25_does_not_pause) {
  StreamSlate slate;

  print_file("NOPAUSE.GCO", "G1 X1\nM250 C1\nG1 X2\n");

  queue.get_available_commands();

  TEST_ASSERT_TRUE_MESSAGE(card.flag.sdprinting, "M250 should not be taken for M25");
}

// A parameter is not a digit, so `M25 P1` is still a pause.
MARLIN_TEST(queue_sd_stream, M25_with_a_parameter_still_pauses) {
  StreamSlate slate;

  print_file("PAUSEP.GCO", "G1 X1\nM25 P1\nG1 X2\n");

  queue.get_available_commands();

  TEST_ASSERT_FALSE_MESSAGE(card.flag.sdprinting, "M25 with a parameter should still pause");
}

/**
 * A file whose last line has no newline still yields that line.
 *
 * Every other line is committed when its terminator arrives; the last one has to be committed
 * by the end of the file instead. Two files differing only in a trailing newline must produce
 * the same commands — which is the relationship, rather than any particular count.
 */
MARLIN_TEST(queue_sd_stream, the_last_line_is_read_with_or_without_a_trailing_newline) {
  const char * const body = "G1 X1\nG1 X2\nG1 X3";

  uint8_t with_newline = 0, without = 0;
  {
    StreamSlate slate;
    char terminated[32];
    snprintf(terminated, sizeof(terminated), "%s\n", body);
    print_file("TERMED.GCO", terminated);
    queue.get_available_commands();
    with_newline = queue.ring_buffer.length;
  }
  {
    StreamSlate slate;
    print_file("UNTERMED.GCO", body);
    queue.get_available_commands();
    without = queue.ring_buffer.length;
  }

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, with_newline, "three lines should give three commands");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(with_newline, without,
    "a missing final newline should not lose the last command");
}

namespace {
  // How many commands a file's worth of lines turns into.
  uint8_t commands_from(const char * const name, const char * const contents) {
    StreamSlate slate;
    print_file(name, contents);
    queue.get_available_commands();
    return queue.ring_buffer.length;
  }
}

/**
 * A line with nothing on it costs nothing.
 *
 * Slicers emit blank lines and comment lines freely, and a queue slot spent on one is a slot
 * not spent on a move. Both are dropped during the read — the comment because the stream
 * state machine discards everything after the ';', the blank line because it ends with
 * nothing accumulated.
 */
MARLIN_TEST(queue_sd_stream, blank_and_comment_lines_cost_no_queue_slots) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, commands_from("PLAIN2.GCO", "G1 X1\nG1 X2\n"),
    "two commands should be two commands");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, commands_from("BLANKS.GCO", "G1 X1\n\n\nG1 X2\n"),
    "blank lines should not be queued");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, commands_from("COMMENT.GCO", "G1 X1\n; a comment\nG1 X2\n"),
    "a whole-line comment should not be queued");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, commands_from("TRAILC.GCO", "G1 X1 ; here\nG1 X2\n"),
    "a trailing comment should not add a command");
}

/**
 * A line of spaces is not an empty line.
 *
 * "Empty" is decided by how many characters were accumulated, and spaces accumulate — so a
 * whitespace-only line is committed and occupies a queue slot, where a genuinely empty one
 * does not. Harmless downstream, since the parser finds no command letter and does nothing,
 * but it is a real difference between two lines that look the same in an editor. Recorded
 * rather than corrected: see the defect register.
 */
MARLIN_TEST(queue_sd_stream, a_whitespace_only_line_still_takes_a_queue_slot) {
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, commands_from("SPACES.GCO", "G1 X1\n   \nG1 X2\n"),
    "a whitespace-only line is committed where an empty one is not");
}

#endif // HAS_MEDIA
