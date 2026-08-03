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
 * Files the machine runs without being asked.
 *
 * On the first mount after boot the firmware looks for `/auto0.g`, runs it, then `/auto1.g`,
 * and so on until one is missing. It is how a machine is configured by its card rather than
 * by its firmware, and nothing in the suite reached it — the whole sequence sat behind an
 * index that no test ever set.
 *
 * The relationship to assert is the sequence itself: which file a call looks for is decided
 * by how many have already run, so running one must move the search on by exactly one, and a
 * gap must end the sequence rather than be stepped over. That is checkable without knowing
 * any particular file name, which is what makes it more than a recording of one run.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA && DISABLED(NO_SD_AUTOSTART)

#include "../test/unit_tests.h"
#include "../support/simulated_media.h"

#include "src/sd/cardreader.h"
#include "src/gcode/queue.h"
#if ENABLED(POWER_LOSS_RECOVERY)
  #include "src/feature/powerloss.h"
#endif

#include <string.h>
#include <stdio.h>

namespace {

  struct AutostartSlate {
    bool was_connected;
    AutostartSlate() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      tidy();
    }
    ~AutostartSlate() { tidy(); MYSERIAL1.host_connected = was_connected; }
    /**
     * The auto-start index, the open file and any queued commands all outlive the test that
     * set them, and a test that fails leaves through a `longjmp` that runs no destructor —
     * so this runs on the way in as well as on the way out.
     */
    static void tidy() {
      card.autofile_cancel();
      card.flag.sdprinting = false;
      card.endFilePrintNow();
      if (card.isFileOpen()) card.closefile();
      if (!card.isMounted()) card.mount();
      card.cdroot();
      TERN_(POWER_LOSS_RECOVERY, recovery.purge());
      queue.clear();
    }
  };

  void put_file(const char * const name, const char * const text) {
    card.openFileWrite(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not create the auto-start file");
    card.write((void*)text, strlen(text));
    card.closefile();
  }

  /**
   * Let the injected commands run.
   *
   * An auto file is not opened directly: `openAndPrintFile()` injects "M23 <name>" and "M24"
   * — the same two commands a host would send — so until the queue is advanced the sequence
   * has decided which file to run but nothing has opened it. Advancing here is what
   * `MarlinCore::loop()` does; the bound is a guard rather than a timeout.
   */
  void run_injected_commands(const uint8_t passes = 8) {
    for (uint8_t i = 0; i < passes; ++i) queue.advance();
  }

  // The card is one image shared by the whole suite, so a test that cares which auto files
  // exist has to say so for all ten rather than only for the ones it creates.
  void clear_autostart_files() {
    for (char i = '0'; i <= '9'; ++i) {
      char name[10];
      sprintf(name, "/auto%c.g", i);
      if (card.fileExists(name)) card.removeFile(name);
    }
  }

}

/**
 * The sequence runs in order, and each file that runs moves it on by one.
 *
 * `autofile_begin()` does not merely arm the sequence — it runs the first file, so beginning
 * and continuing are the same step seen twice. Asserting the *name of the file that opened*
 * after each one, rather than only that something opened, is what ties the index to the file
 * it selects: an implementation that ran `/auto0.g` three times would satisfy a test that
 * only counted successes.
 */
MARLIN_TEST(media_autostart, the_auto_files_run_in_index_order) {
  AutostartSlate slate;
  clear_autostart_files();

  put_file("/auto0.g", "M117 zero\n");
  put_file("/auto1.g", "M117 one\n");
  put_file("/auto2.g", "M117 two\n");

  card.autofile_begin();
  run_injected_commands();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("AUTO0.G", card.filename, "beginning should run the first auto file");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, card.autofile_index, "a file that ran should move the index on by one");

  for (uint8_t i = 1; i < 3; ++i) {
    card.endFilePrintNow();
    TEST_ASSERT_TRUE_MESSAGE(card.autofile_check(), "an existing auto file should have started");
    run_injected_commands();
    char want[10];
    sprintf(want, "AUTO%c.G", '0' + i);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, card.filename, "the wrong auto file was started");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(i + 2, card.autofile_index, "the index should track the files that have run");
  }
}

/**
 * A missing file ends the sequence rather than being stepped over.
 *
 * With `/auto0.g` and `/auto2.g` present but no `/auto1.g`, the run must stop after the
 * first. This is the boundary that says the index is a cursor into a contiguous sequence and
 * not a search: the same card would produce two starts if the firmware skipped gaps.
 */
MARLIN_TEST(media_autostart, a_gap_ends_the_sequence) {
  AutostartSlate slate;
  clear_autostart_files();

  put_file("/auto0.g", "M117 zero\n");
  put_file("/auto2.g", "M117 two\n");

  card.autofile_begin();
  run_injected_commands();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("AUTO0.G", card.filename, "/auto0.g should have run");
  card.endFilePrintNow();

  TEST_ASSERT_FALSE_MESSAGE(card.autofile_check(), "the sequence should stop at the missing /auto1.g");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, card.autofile_index, "a stopped sequence should cancel itself");

  // ...and stays stopped, so /auto2.g is never reached.
  TEST_ASSERT_FALSE_MESSAGE(card.autofile_check(), "a cancelled sequence should not resume");
}

// An index of zero means no sequence is running, whatever is on the card.
MARLIN_TEST(media_autostart, nothing_runs_until_the_sequence_begins) {
  AutostartSlate slate;
  clear_autostart_files();

  put_file("/auto0.g", "M117 zero\n");

  TEST_ASSERT_EQUAL_UINT8(0, card.autofile_index);
  TEST_ASSERT_FALSE_MESSAGE(card.autofile_check(), "no auto file should run before the sequence begins");

  card.autofile_begin();
  run_injected_commands();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("AUTO0.G", card.filename, "beginning the sequence should run /auto0.g");
}

// With nothing on the card the sequence ends where it starts.
MARLIN_TEST(media_autostart, an_empty_card_cancels_the_sequence_immediately) {
  AutostartSlate slate;
  clear_autostart_files();

  card.autofile_begin();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, card.autofile_index, "with no /auto0.g the sequence should cancel");
  TEST_ASSERT_FALSE_MESSAGE(card.isPrinting(), "nothing should be printing");
}

#if ENABLED(POWER_LOSS_RECOVERY)

  /**
   * An interrupted print outranks the card's start-up files.
   *
   * Both want the machine as soon as it mounts, and running a configuration file first would
   * move the machine before it could resume — so a valid recovery record suppresses the
   * sequence entirely. The pair of assertions matters more than either: the same card and the
   * same call must give opposite answers according to the record alone.
   */
  MARLIN_TEST(media_autostart, a_recoverable_job_suppresses_the_auto_files) {
    AutostartSlate slate;
    clear_autostart_files();

    put_file("/auto0.g", "M117 zero\n");
    put_file("resume.gco", "G28\n");

    strncpy(recovery.info.sd_filename, "resume.gco", sizeof(recovery.info.sd_filename) - 1);
    recovery.info.sd_filename[sizeof(recovery.info.sd_filename) - 1] = '\0';
    recovery.save(true);
    recovery.load();
    TEST_ASSERT_TRUE_MESSAGE(recovery.valid(), "the fixture failed to leave a resumable job");

    card.autofile_begin();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, card.autofile_index,
      "a resumable job should suppress the auto files");

    recovery.purge();
    card.autofile_begin();
    run_injected_commands();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("AUTO0.G", card.filename,
      "with the record gone the same card should run the auto files");
  }

#endif // POWER_LOSS_RECOVERY

#endif // HAS_MEDIA && !NO_SD_AUTOSTART
