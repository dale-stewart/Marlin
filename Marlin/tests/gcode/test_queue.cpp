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

// A comment line carries no command.
MARLIN_TEST(queue, a_comment_line_is_not_queued) {
  CleanQueue clean;
  const int16_t before = motion.feedrate_percentage;
  host_transmits("; M220 S5");
  drain_queue();
  TEST_ASSERT_EQUAL(before, motion.feedrate_percentage);
}
