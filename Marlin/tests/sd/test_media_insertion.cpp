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
 * Taking the card out, and putting it back.
 *
 * `CardReader::manage_media()` was the largest survivor cluster in `cardreader.cpp` — 66 of 397,
 * on a mutation run where the file was 86% covered. Every test in this tree starts with the card
 * already in the slot and leaves it there, so the state machine that notices a card arriving or
 * leaving had never been given anything to notice. Coverage saw it run on every idle; mutation
 * saw that changing what it decided made no difference to anything.
 *
 * The slot is a pin. `HAS_SD_DETECT` is set because this board defines `SD_DETECT_PIN`, and
 * `isSDCardInserted()` is `READ(SD_DETECT_PIN) == SD_DETECT_STATE` — LOW here. A simulated pin
 * reads LOW at reset, which is why the card has always appeared present without anybody saying
 * so. Driving it HIGH is the user pulling the card out.
 *
 * The route in is `marlin.idle()`, because that is where `MarlinCore` calls `manage_media()`.
 * Calling the method directly would work and would test a path no printer takes.
 */

#include "src/inc/MarlinConfig.h"

#if ALL(HAS_MEDIA, HAS_SD_DETECT)

#include "../test/unit_tests.h"
#include "../support/simulated_media.h"
#include "../support/simulated_machine.h"
#include "src/sd/cardreader.h"
#include "src/gcode/queue.h"
#include "src/MarlinCore.h"

#include <string.h>

namespace {

  /**
   * A machine with the card in the slot, restored on the way *in* as well as out.
   *
   * The pin is the important part. A test that fails after pulling the card leaves through a
   * `longjmp` that runs no destructor, and every later test would then run against a machine
   * whose card is gone — which presents as unrelated media tests failing to open files they
   * just wrote. Setting it here as well as in the harness teardown is belt and braces for the
   * one piece of state that would poison the most.
   */
  struct CardInTheSlot {
    bool was_connected;
    CardInTheSlot() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      tidy();
    }
    ~CardInTheSlot() { tidy(); MYSERIAL1.host_connected = was_connected; }
    static void tidy() {
      WRITE(SD_DETECT_PIN, SD_DETECT_STATE);      // card present
      card.flag.sdprinting = false;
      card.endFilePrintNow();
      if (card.isFileOpen()) card.closefile();
      if (!card.isMounted()) card.mount();
      card.cdroot();
      queue.clear();
    }
  };

  void put_file(const char * const name, const char * const text) {
    card.openFileWrite(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not create the file");
    card.write((void*)text, strlen(text));
    card.closefile();
  }

  // The card is noticed on an idle pass, which is where MarlinCore calls manage_media().
  void let_the_machine_notice(const uint8_t passes = 4) {
    for (uint8_t i = 0; i < passes; ++i) marlin.idle();
  }

  void pull_the_card()   { WRITE(SD_DETECT_PIN, !SD_DETECT_STATE); }
  void insert_the_card() { WRITE(SD_DETECT_PIN, SD_DETECT_STATE); }

}

/**
 * Inserting a card mounts it — so the state machine does run and does act.
 *
 * This one is the control for the two below it. Without it, "removal does nothing" is equally
 * well explained by `manage_media()` never being reached at all, and the tests would be pinning
 * the fixture rather than the firmware.
 */
MARLIN_TEST(media_insertion, inserting_a_card_mounts_it) {
  CardInTheSlot slot;

  pull_the_card();
  let_the_machine_notice();       // lets prev_stat settle on "absent"
  card.release();
  TEST_ASSERT_FALSE_MESSAGE(card.isMounted(), "the fixture did not get to an unmounted state");

  insert_the_card();
  let_the_machine_notice();

  TEST_ASSERT_TRUE_MESSAGE(card.isMounted(),
    "a card appearing in the slot should be mounted without a command being sent - and this "
    "passing is what makes the two tests below claims about the firmware rather than the fixture");
}

/**
 * LEGACY-BEHAVIOR: taking the card out does **not** release it. See defect register #68.
 *
 * `manage_media()` has a branch for media leaving the slot, and on a single-volume machine it
 * cannot be taken. The `else` is reached only when `stat == INSERT_NONE`, and the condition
 * guarding it *is* `stat`:
 *
 *     const bool did_insert = TERN(HAS_MULTI_VOLUME, vadd, stat) != INSERT_NONE;
 *     if (did_insert) { ... }
 *     else if ( TERN(HAS_MULTI_VOLUME, (...), stat) ) { release(); }
 *
 * So `release()` is never called, `flag.mounted` stays true, and the firmware goes on believing
 * in a volume that has been physically removed. Asserted as it behaves rather than as it should,
 * per the rescue's rule that a discovered defect is recorded and pinned, not quietly fixed.
 */
MARLIN_TEST(media_insertion, a_removed_card_is_not_released) {
  CardInTheSlot slot;
  TEST_ASSERT_TRUE_MESSAGE(card.isMounted(), "the card should start out mounted");

  pull_the_card();
  TEST_ASSERT_FALSE_MESSAGE(card.isSDCardInserted(),
    "the detect line should report the card gone - if this fails the fixture is at fault, not "
    "the firmware");

  let_the_machine_notice();

  TEST_ASSERT_TRUE_MESSAGE(card.isMounted(),
    "LEGACY-BEHAVIOR: the volume is still mounted after the card was removed, because the "
    "removal branch cannot be reached on a single-volume machine - see register #68");
}

/**
 * LEGACY-BEHAVIOR: a card pulled during a print does not stop the print. Register #68.
 *
 * This is the consequence that matters, and it is the reason the branch is there at all:
 * `release()` calls `abortFilePrintSoon()` when a print is running. With `release()` unreachable
 * the request is never made, so the machine keeps executing a job whose source has gone —
 * reading further blocks from a slot that is empty, which on real hardware returns whatever the
 * bus floats to rather than an error.
 */
MARLIN_TEST(media_insertion, a_card_pulled_during_a_print_does_not_stop_it) {
  CardInTheSlot slot;
  put_file("job.gco", "G1 X1\nG1 X2\nG1 X3\nG1 X4\n");

  card.openFileRead("job.gco");
  card.startOrResumeFilePrinting();
  TEST_ASSERT_TRUE_MESSAGE(card.isStillPrinting(), "the fixture did not get a print running");

  pull_the_card();
  let_the_machine_notice();

  TEST_ASSERT_FALSE_MESSAGE(card.flag.abort_sd_printing,
    "LEGACY-BEHAVIOR: no abort is requested when the card is pulled mid-print, because the "
    "code that would request it is unreachable - register #68");
  TEST_ASSERT_TRUE_MESSAGE(card.isStillPrinting(),
    "and the print is still considered to be running, with its source no longer in the machine");
}

#endif // HAS_MEDIA && HAS_SD_DETECT
