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
