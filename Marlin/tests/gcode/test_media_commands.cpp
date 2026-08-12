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
 * Driving the card from the host, the way a print does.
 *
 * These go in through `process_parsed_command()` rather than calling `CardReader`
 * directly, because the commands *are* the feature: a host sends `M23` to choose a file
 * and `M24` to start it, and what the operator sees is the reply. Asserting on the reply
 * also keeps the tests honest about the parts a direct call would skip — argument
 * parsing, the reporting, and the interaction between commands.
 *
 * The card is the simulated one, formatted FAT16 in memory, so the file operations run
 * through the firmware's own filesystem.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "../test/unit_tests.h"
#include "../support/simulated_media.h"
#include "serial_capture.h"

#include "src/sd/cardreader.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/gcode/queue.h"
#include "src/module/printcounter.h"
#if ENABLED(POWER_LOSS_RECOVERY)
  #include "src/feature/powerloss.h"
#endif

#include <string.h>

namespace {

  /**
   * Put the card back the way it was found, and keep the port quiet meanwhile.
   *
   * The `host_connected` flag is not decoration. `HalSerial::write()` spins while the
   * 128-byte transmit buffer is full and a host is believed to be listening, and nothing
   * drains it outside a `SerialCapture`. The media layer reports freely — opening a file
   * for writing sends a host notification on its own — so any setup done outside a
   * capture will hang the run unless the port is told nobody is there.
   */
  struct MediaSlate {
    bool was_connected;
    MediaSlate() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      tidy();
    }
    ~MediaSlate() { tidy(); MYSERIAL1.host_connected = was_connected; }
    /**
     * `M524` only *requests* an abort — it sets `abort_sd_printing`, which the main loop
     * is supposed to act on, and nothing runs the main loop here. Left set, that flag
     * makes `isStillPrinting()` false for every later test, so the next print looks like
     * it never started. `endFilePrintNow()` is the firmware's own way to finish the job
     * on the spot: it clears the flag and closes the file.
     */
    static void tidy() {
      card.flag.sdprinting = false;
      card.endFilePrintNow();
      if (card.isFileOpen()) card.closefile();
      if (!card.isMounted()) card.mount();
    }
  };

  void send(const char * const line) {
    char buf[96];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  // Reply to a command, with the port pretending a host is listening so output is kept.
  std::string reply_to(const char * const line) {
    SerialCapture capture;
    send(line);
    return capture.finish();
  }

  /**
   * Names here must fit 8.3, because this build has no long-filename support.
   * A longer stem is silently stored mangled, and the name used to write it will then
   * not open it again — which presents as a file that vanishes rather than as an error.
   */
  void put_file(const char * const name, const char * const text) {
    card.openFileWrite(name);
    card.write((void*)text, strlen(text));
    card.closefile();
  }

  /**
   * A print running from the card, which is a different state from a file being open.
   *
   * Four commands and the queue's whole media branch only do anything while
   * `card.isStillPrinting()` — the queue refuses to fetch otherwise
   * (`if (!card.isStillFetching()) return;`), so selecting a file is not enough to reach
   * any of it. `M23` then `M24` is the sequence a host uses, and using the commands
   * rather than `startOrResumeFilePrinting()` keeps the fixture on the same path a real
   * print takes.
   */
  void print_from_card(const char * const name, const char * const gcode) {
    put_file(name, gcode);
    char select[64];
    snprintf(select, sizeof(select), "M23 %s", name);
    send(select);
    send("M24");
  }

  // Let the queue pull whatever the file has for it, without running the commands.
  size_t fetch_queued_commands() {
    queue.get_available_commands();
    return queue.ring_buffer.length;
  }

  /**
   * Run the print to the end of the file.
   *
   * Fetching and advancing is what `MarlinCore::loop()` does; doing it here keeps the
   * clock and the interrupts under the test's control. The bound is a guard, not a
   * timeout — a print of a handful of lines that has not finished in a thousand passes is
   * not going to, and returning rather than spinning turns a firmware change that breaks
   * this into a failed assertion instead of a hung suite.
   */
  bool run_print_to_completion(const uint16_t max_passes = 1000) {
    for (uint16_t i = 0; i < max_passes; ++i) {
      if (!card.isStillFetching() && queue.ring_buffer.empty()) return true;
      queue.get_available_commands();
      queue.advance();
    }
    return false;
  }

}

// M21 mounts the card; M22 releases it. The pair is what a host uses to swap media.
MARLIN_TEST(media_commands, M21_mounts_the_card_and_M22_releases_it) {
  MediaSlate slate;

  send("M22");
  TEST_ASSERT_FALSE_MESSAGE(card.isMounted(), "M22 did not release the card");

  send("M21");
  TEST_ASSERT_TRUE_MESSAGE(card.isMounted(), "M21 did not mount the card");
}

/**
 * M20 lists what is on the card.
 *
 * The listing is how a host populates its file chooser, so the assertion is that a file
 * written a moment ago appears in it — not that the listing has any particular shape.
 */
MARLIN_TEST(media_commands, M20_lists_the_files_on_the_card) {
  MediaSlate slate;
  put_file("listed.gco", "G28\n");

  const std::string reply = reply_to("M20");

  TEST_ASSERT_TRUE_MESSAGE(reply.find("Begin file list") != std::string::npos,
    "M20 did not open a file list");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("LISTED.GCO") != std::string::npos,
    "a file on the card was missing from the listing");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("End file list") != std::string::npos,
    "M20 did not close the file list");
}

/**
 * M23 selects a file and reports what it found.
 *
 * The size in the reply is the file's own, so a host can show progress against it. It is
 * asserted against the length actually written rather than a literal, which is what makes
 * this a test of the selection rather than of this test's arithmetic.
 */
MARLIN_TEST(media_commands, M23_selects_a_file_and_reports_its_size) {
  MediaSlate slate;
  const char * const contents = "G28\nG1 X10\nG1 Y10\n";
  put_file("chosen.gco", contents);

  const std::string reply = reply_to("M23 chosen.gco");

  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "M23 did not open the file");
  TEST_ASSERT_EQUAL_UINT32(strlen(contents), card.getFileSize());
  TEST_ASSERT_TRUE_MESSAGE(reply.find("File opened") != std::string::npos,
    "M23 did not report opening the file");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("File selected") != std::string::npos,
    "M23 did not report selecting the file");
}

// Choosing a file that is not there must say so rather than leaving a stale selection.
MARLIN_TEST(media_commands, M23_reports_a_file_that_is_not_there) {
  MediaSlate slate;

  const std::string reply = reply_to("M23 missing.gco");

  TEST_ASSERT_FALSE_MESSAGE(card.isFileOpen(), "a missing file was reported as open");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("open failed") != std::string::npos,
    "M23 did not report the failure to open");
}

/**
 * M27 reports how far through the file the print has got.
 *
 * The position is asserted after moving it with M26, so the reply has to reflect the
 * state rather than a constant: a report that always said zero would pass against a
 * freshly opened file.
 */
MARLIN_TEST(media_commands, M27_reports_the_position_M26_set) {
  MediaSlate slate;
  put_file("progress.gco", "G28\nG1 X10\nG1 Y10\nG1 Z10\n");
  send("M23 progress.gco");

  send("M26 S8");
  const std::string reply = reply_to("M27");

  TEST_ASSERT_EQUAL_UINT32(8, card.getIndex());
  TEST_ASSERT_TRUE_MESSAGE(reply.find("SD printing byte 8/") != std::string::npos,
    "M27 did not report the position M26 set");
}

// With nothing selected there is nothing to report, and saying so is the useful answer.
MARLIN_TEST(media_commands, M27_says_when_nothing_is_printing) {
  MediaSlate slate;
  if (card.isFileOpen()) card.closefile();

  const std::string reply = reply_to("M27");

  TEST_ASSERT_TRUE_MESSAGE(reply.find("Not SD printing") != std::string::npos,
    "M27 did not say the card was idle");
}

// M30 deletes a file, which is how a host tidies up after uploading.
MARLIN_TEST(media_commands, M30_deletes_a_file) {
  MediaSlate slate;
  put_file("doomed.gco", "M105\n");
  TEST_ASSERT_TRUE(card.fileExists("doomed.gco"));

  send("M30 doomed.gco");

  TEST_ASSERT_FALSE_MESSAGE(card.fileExists("doomed.gco"), "M30 left the file on the card");
}

/**
 * M28 opens a file for writing and M29 closes it, which is how a host uploads.
 *
 * The round trip is the point: between the two, ordinary commands are diverted into the
 * file instead of being executed, so what comes back out has to be what was sent in.
 */
MARLIN_TEST(media_commands, M28_and_M29_write_a_file_to_the_card) {
  MediaSlate slate;

  send("M28 upload.gco");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "M28 did not open a file for writing");
  TEST_ASSERT_TRUE_MESSAGE(card.flag.saving, "M28 did not put the card into saving mode");

  send("M29");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.saving, "M29 did not leave saving mode");
  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("upload.gco"), "the uploaded file is not on the card");
}

#if ENABLED(BINARY_FILE_TRANSFER)

/**
 * `M28 B<flag>` is the binary-transfer variant (`BINARY_FILE_TRANSFER`): the B parameter
 * is parsed out of the raw argument by hand, ahead of the filename, rather than through
 * `parser`'s normal letter/value machinery. `B1` switches the port into binary mode
 * instead of opening a file — the file only opens once the transfer itself begins
 * elsewhere — so the sign of B decides which of the two ever happens.
 */
MARLIN_TEST(media_commands, M28_B1_switches_to_binary_mode_without_opening_a_file) {
  MediaSlate slate;
  card.flag.binary_mode = false;

  const std::string reply = reply_to("M28 B1 upload.gco");

  TEST_ASSERT_TRUE_MESSAGE(card.flag.binary_mode, "M28 B1 did not switch to binary mode");
  TEST_ASSERT_FALSE_MESSAGE(card.isFileOpen(), "M28 B1 opened a file instead of switching modes");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("Switching to Binary Protocol") != std::string::npos,
    "M28 B1 did not announce the switch to binary mode");

  card.flag.binary_mode = false;
}

// `B0` is explicitly non-binary, so parsing still has to skip past it to reach the filename.
MARLIN_TEST(media_commands, M28_B0_opens_the_file_named_after_the_flag) {
  MediaSlate slate;
  card.flag.binary_mode = false;

  send("M28 B0 flagged.gco");

  TEST_ASSERT_FALSE_MESSAGE(card.flag.binary_mode, "M28 B0 left the card in binary mode");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "M28 B0 did not open the named file");
  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("flagged.gco"), "M28 B0 opened the wrong file");
}

/**
 * The flag test is `p[0] == 'B'`, not "starts with a letter near B" — only an exact 'B'
 * counts, and even then only paired with a numeric digit right after it. Anything else is
 * an ordinary filename, first character and all.
 */
MARLIN_TEST(media_commands, M28_only_an_exact_B_with_a_digit_after_it_is_the_flag) {
  MediaSlate slate;

  // A letter below 'B' with a digit after it is still just a filename.
  card.flag.binary_mode = false;
  send("M28 A1lower.gco");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.binary_mode, "a filename starting below 'B' was read as the binary flag");
  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("a1lower.gco"), "the filename was parsed wrong");
  card.closefile();

  // A letter above 'B' with a digit after it is also just a filename.
  card.flag.binary_mode = false;
  send("M28 C1higher.gco");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.binary_mode, "a filename starting above 'B' was read as the binary flag");
  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("c1higher.gco"), "the filename was parsed wrong");
  card.closefile();

  // 'B' with a non-numeric character after it is not the flag either.
  card.flag.binary_mode = false;
  send("M28 Bxflag.gco");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.binary_mode, "'B' without a following digit was read as the binary flag");
  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("bxflag.gco"), "the filename was parsed wrong");

  card.flag.binary_mode = false;
}

// The B flag is exactly two characters (letter + digit); parsing must not eat into the name.
MARLIN_TEST(media_commands, M28_B_flag_is_exactly_two_characters_wide) {
  MediaSlate slate;

  send("M28 B0adjacent.gco");

  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("adjacent.gco"),
    "the flag parse consumed part of a filename that touched it");
}

// Any number of spaces between the flag and the filename are skipped, not just the first.
MARLIN_TEST(media_commands, M28_B_flag_skips_every_space_before_the_filename) {
  MediaSlate slate;

  send("M28 B0   gapped.gco");

  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("gapped.gco"),
    "not every space between the flag and the filename was skipped");
}

#endif // BINARY_FILE_TRANSFER

//
// ---- With a print actually running from the card ----
//

// M24 starts the selected file printing; until it does, the card is merely open.
MARLIN_TEST(media_commands, M24_starts_the_selected_file_printing) {
  MediaSlate slate;
  put_file("run.gco", "G91\nG90\n");

  send("M23 run.gco");
  TEST_ASSERT_TRUE(card.isFileOpen());
  TEST_ASSERT_FALSE_MESSAGE(card.isPrinting(), "selecting a file started a print on its own");

  send("M24");
  TEST_ASSERT_TRUE_MESSAGE(card.isStillPrinting(), "M24 did not start the print");

}

/**
 * M25 pauses without giving up the file, which is what makes resuming possible.
 *
 * Paused is its own state — `isPaused()` is "the file is open but not printing" — and it
 * is the distinction that separates a pause from an abort. Asserting all three of open,
 * not printing, and paused says which state it is rather than only that it changed.
 */
MARLIN_TEST(media_commands, M25_pauses_the_print_without_closing_the_file) {
  MediaSlate slate;
  print_from_card("pausable.gco", "G90\nG90\nG90\n");

  send("M25");

  TEST_ASSERT_FALSE_MESSAGE(card.isPrinting(), "M25 did not stop the print");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "M25 closed the file");
  TEST_ASSERT_TRUE_MESSAGE(card.isPaused(), "the print is neither running nor paused");

}

/**
 * The queue reads the print from the card.
 *
 * This is the media half of `GCodeQueue`, and it is only reachable while a print is
 * running — `get_sdcard_commands()` returns immediately unless `isStillFetching()`. The
 * position advancing is the evidence the bytes came off the card rather than from
 * anywhere else.
 */
MARLIN_TEST(media_commands, the_queue_fetches_commands_from_the_running_file) {
  MediaSlate slate;
  print_from_card("queued.gco", "G90\nG91\nG90\n");
  queue.clear();

  TEST_ASSERT_EQUAL_UINT32(0, card.getIndex());
  const size_t queued = fetch_queued_commands();

  TEST_ASSERT_GREATER_THAN_MESSAGE(0, queued, "nothing was fetched from the running file");
  TEST_ASSERT_GREATER_THAN_MESSAGE(0, card.getIndex(), "the file position did not advance");

  queue.clear();
}

// Nothing is fetched from a file that is merely selected — the gate is the print, not the
// open file, which is why every command below needed this fixture.
MARLIN_TEST(media_commands, the_queue_ignores_a_file_that_is_not_printing) {
  MediaSlate slate;
  put_file("idle.gco", "G90\nG90\n");
  send("M23 idle.gco");
  queue.clear();

  TEST_ASSERT_EQUAL_MESSAGE(0, fetch_queued_commands(), "a file that is not printing was fetched");
  TEST_ASSERT_EQUAL_UINT32(0, card.getIndex());
}

// M32 does M23 and M24 in one, which is how a host starts a print in a single command.
MARLIN_TEST(media_commands, M32_selects_and_starts_in_one_command) {
  MediaSlate slate;
  put_file("oneshot.gco", "G90\nG90\n");

  send("M32 oneshot.gco");

  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "M32 did not open the file");
  TEST_ASSERT_TRUE_MESSAGE(card.isStillPrinting(), "M32 did not start printing");

}

/**
 * M524 aborts the print.
 *
 * The abort is requested rather than performed on the spot — `abort_sd_printing` is a
 * flag the main loop acts on — so what the command guarantees is that the print stops
 * being one that should still be fetched from. `isStillPrinting()` is exactly that
 * question, which is why it is the thing asserted.
 */
MARLIN_TEST(media_commands, M524_abandons_the_print) {
  MediaSlate slate;
  print_from_card("abort.gco", "G90\nG90\nG90\n");
  TEST_ASSERT_TRUE(card.isStillPrinting());

  send("M524");

  TEST_ASSERT_FALSE_MESSAGE(card.isStillPrinting(), "M524 left the print running");
}

/**
 * A print that reaches the end of the file stops being one.
 *
 * `fileHasFinished()` closes the file, sets `sdprintdone` so no more bytes are fetched,
 * and flags the machine so the main loop will enqueue M1001. Running the commands out of
 * the file rather than seeking to the end is what makes this the real path: the queue has
 * to notice the end while it is reading, not be told about it.
 */
MARLIN_TEST(media_commands, a_print_that_reaches_the_end_of_the_file_finishes) {
  MediaSlate slate;
  print_from_card("short.gco", "G90\nG91\nG90\n");
  queue.clear();

  TEST_ASSERT_TRUE_MESSAGE(run_print_to_completion(), "the print never reached the end of the file");

  TEST_ASSERT_TRUE_MESSAGE(card.flag.sdprintdone, "the print did not record itself as done");
  TEST_ASSERT_FALSE_MESSAGE(card.isStillFetching(), "the queue would still fetch from a finished print");
  TEST_ASSERT_FALSE_MESSAGE(card.isFileOpen(), "the finished print left its file open");

  queue.clear();
}

/**
 * M1001 is what actually ends the job, and it is the reason a finished print is safe to
 * power off.
 *
 * It is enqueued once the file runs out, and it does the tidying the rest of the firmware
 * depends on: the recovery record is purged, so the next power-on does not offer to
 * resume a print that already finished, and the job timer stops. Both are asserted
 * because either alone would leave the machine wrong in a way the operator would see.
 */
MARLIN_TEST(media_commands, M1001_purges_the_recovery_record_and_stops_the_clock) {
  MediaSlate slate;
  print_from_card("finish.gco", "G90\nG90\n");
  queue.clear();

  print_job_timer.start();
  #if ENABLED(POWER_LOSS_RECOVERY)
    recovery.save(true);
    TEST_ASSERT_TRUE_MESSAGE(recovery.exists(), "no recovery record to purge");
  #endif

  TEST_ASSERT_TRUE(run_print_to_completion());
  send("M1001");

  #if ENABLED(POWER_LOSS_RECOVERY)
    TEST_ASSERT_FALSE_MESSAGE(recovery.exists(),
      "a finished print left a recovery record, so the next boot would offer to resume it");
  #endif
  TEST_ASSERT_FALSE_MESSAGE(print_job_timer.isRunning(), "the job timer is still running");
  TEST_ASSERT_FALSE_MESSAGE(card.isPrinting(), "the card still believes it is printing");

  queue.clear();
}

/**
 * M928 starts logging what follows into a file on the card.
 *
 * The same machinery as M28 — the card in saving mode — reached by a different command,
 * and what a host uses to capture a session rather than upload a print.
 *
 * Note where the *end* of a log lives, because it is not where it looks. `M29`'s handler
 * is one line, `card.flag.saving = false;`, and it does not touch `flag.logging`. Logging
 * is ended by `CardReader::closefile()`, which the queue's write path calls when it sees
 * M29 arrive *in the command stream* — the branch at `queue.cpp:687` that decides whether
 * a line is written to the file or executed. So a directly dispatched `M29` cannot end a
 * log, and this test closes the file the way the firmware does instead of pretending the
 * command did it.
 */
MARLIN_TEST(media_commands, M928_starts_logging_to_a_file) {
  MediaSlate slate;

  send("M928 log.gco");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "M928 did not open a log file");
  TEST_ASSERT_TRUE_MESSAGE(card.flag.logging, "M928 did not put the card into logging mode");
  TEST_ASSERT_TRUE_MESSAGE(card.flag.saving, "logging did not imply saving");

  card.closefile();
  TEST_ASSERT_FALSE_MESSAGE(card.flag.logging, "closing the file left the card logging");
  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("log.gco"), "the log file is not on the card");
}

// M29 clears the saving flag and nothing else — the rest of ending an upload happens in
// the queue, which is why this asserts so little.
MARLIN_TEST(media_commands, M29_clears_the_saving_flag) {
  MediaSlate slate;

  send("M28 up2.gco");
  TEST_ASSERT_TRUE(card.flag.saving);

  send("M29");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.saving, "M29 did not clear the saving flag");
}

#endif // HAS_MEDIA
