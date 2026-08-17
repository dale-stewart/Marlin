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
 * Taking the card out releases it, so nothing goes on reading from a slot that is empty.
 *
 * Everything the firmware knows about the volume — the mounted flag, the working directory, the
 * item count — describes a card that is no longer there. Continuing to trust it means reading
 * blocks from a device that will answer with whatever the bus floats to, which is worse than
 * failing, because it looks like data.
 */
MARLIN_TEST(media_insertion, taking_the_card_out_releases_it) {
  CardInTheSlot slot;
  TEST_ASSERT_TRUE_MESSAGE(card.isMounted(), "the card should start out mounted");

  pull_the_card();
  let_the_machine_notice();

  TEST_ASSERT_FALSE_MESSAGE(card.isMounted(),
    "a card that has been removed should not still be mounted - every later read would be of "
    "an empty slot, and an empty slot answers rather than refusing");
}

/**
 * Putting it back mounts it again, without a command being sent.
 *
 * The pair matters more than either half. A firmware that released on removal and never
 * remounted would be indistinguishable from one that worked, right up until the user put the
 * card back and found the printer had stopped seeing it — and the fix, power-cycling, hides the
 * fault from whoever might have reported it.
 */
MARLIN_TEST(media_insertion, putting_it_back_mounts_it_again) {
  CardInTheSlot slot;

  pull_the_card();
  let_the_machine_notice();
  TEST_ASSERT_FALSE(card.isMounted());

  insert_the_card();
  let_the_machine_notice();

  TEST_ASSERT_TRUE_MESSAGE(card.isMounted(),
    "a card put back in should be mounted again on its own - no command is sent when someone "
    "pushes a card into a slot");
}

/**
 * A card pulled during a print aborts the print rather than letting it run on.
 *
 * This is the one with consequences. The job is being read from the card a few hundred bytes at
 * a time, so a print whose source has gone is a machine executing whatever it already had
 * buffered and then stopping mid-move with the heaters on. `release()` asks for the abort — the
 * flag rather than the act, because the queue has to unwind first — and that request is what is
 * asserted here.
 */
MARLIN_TEST(media_insertion, a_card_pulled_during_a_print_aborts_it) {
  CardInTheSlot slot;
  put_file("job.gco", "G1 X1\nG1 X2\nG1 X3\nG1 X4\n");

  card.openFileRead("job.gco");
  card.startOrResumeFilePrinting();
  TEST_ASSERT_TRUE_MESSAGE(card.isStillPrinting(), "the fixture did not get a print running");

  pull_the_card();
  let_the_machine_notice();

  TEST_ASSERT_TRUE_MESSAGE(card.flag.abort_sd_printing,
    "pulling the card during a print should ask for the print to be abandoned - the source of "
    "the job is gone, and what is left in the buffer is a few moves and then silence with the "
    "heaters still on");
  TEST_ASSERT_FALSE_MESSAGE(card.isMounted(), "and the volume should be released with it");
}

#endif // HAS_MEDIA && HAS_SD_DETECT
