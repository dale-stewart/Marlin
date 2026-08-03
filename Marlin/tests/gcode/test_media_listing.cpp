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
 * What a listing shows, and what it leaves out.
 *
 * `M20` had one test: write a file, list, find its name. That is enough to say the command
 * runs and is not enough to say anything about the decision it makes for every entry —
 * `is_visible_entity()`, which accepts directories and G-code files and rejects everything
 * else. Mutation said so: the extension checks were among the largest surviving clusters in
 * `cardreader.cpp`, because the card had only ever held files that were meant to be listed.
 * A filter is only tested by something it is supposed to reject.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "../test/unit_tests.h"
#include "../support/simulated_media.h"
#include "serial_capture.h"

#include "src/sd/cardreader.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"

#include <string.h>
#include <string>

namespace {

  struct ListingSlate {
    bool was_connected;
    ListingSlate() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      if (card.isFileOpen()) card.closefile();
      if (!card.isMounted()) card.mount();
      card.cdroot();                    // listings are relative to the working directory
    }
    ~ListingSlate() {
      if (card.isFileOpen()) card.closefile();
      card.cdroot();
      MYSERIAL1.host_connected = was_connected;
    }
  };

  void send(const char * const line) {
    char buf[96];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

  std::string reply_to(const char * const line) {
    SerialCapture capture;
    send(line);
    return capture.finish();
  }

  void put_file(const char * const name, const char * const text) {
    card.openFileWrite(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not create the file");
    card.write((void*)text, strlen(text));
    card.closefile();
  }

  bool listed(const std::string &reply, const char * const name) {
    return reply.find(name) != std::string::npos;
  }

}

/**
 * The listing is a filter on the extension, not a directory dump.
 *
 * Four files, differing only in what follows the dot, and the firmware is expected to
 * accept exactly the one class it prints from. The rejected names are what give the test
 * its power: with only the accepted one present, an implementation that lists everything
 * passes.
 *
 * `.G~` is the interesting rejection. It is not simply "not a G-code file" — the check is
 * that the extension *starts* with 'G' and its second character is not '~', so a backup
 * left by an editor is skipped while `.GCO` and `.G` are kept. One character apart, and on
 * opposite sides of the decision.
 */
MARLIN_TEST(media_listing, M20_lists_gcode_files_and_skips_everything_else) {
  ListingSlate slate;

  put_file("SHOWN.GCO", "G28\n");     // accepted: extension starts with G
  put_file("PLAIN.G",   "G28\n");     // accepted: bare .G
  put_file("BACKUP.G~", "G28\n");     // rejected: a backup, second character is '~'
  put_file("README.TXT","hello\n");   // rejected: not a G-code extension

  const std::string reply = reply_to("M20");

  TEST_ASSERT_TRUE_MESSAGE(listed(reply, "SHOWN.GCO"), "a .GCO file should be listed");
  TEST_ASSERT_TRUE_MESSAGE(listed(reply, "PLAIN.G"), "a bare .G file should be listed");
  TEST_ASSERT_FALSE_MESSAGE(listed(reply, "BACKUP.G~"), "a .G~ backup should not be listed");
  TEST_ASSERT_FALSE_MESSAGE(listed(reply, "README.TXT"), "a non-G-code file should not be listed");
}

/**
 * A listing walks the whole tree, and says where each file is.
 *
 * The path a host gets back has to be one it can send straight to `M23`, so the directories
 * traversed to reach a file are prepended to its name. Asserting the joined form rather
 * than the bare name is what distinguishes a real recursive listing from one that recurses
 * and then forgets where it has been.
 */
MARLIN_TEST(media_listing, M20_lists_files_inside_subdirectories_with_their_path) {
  ListingSlate slate;

  MediaFile root = card.getroot(), made;
  made.mkdir(&root, "LISTSUB");
  made.close();

  card.cd("LISTSUB");
  put_file("NESTED.GCO", "G28\n");
  card.cdroot();

  const std::string reply = reply_to("M20");

  TEST_ASSERT_TRUE_MESSAGE(listed(reply, "LISTSUB/NESTED.GCO"),
    "a file in a subdirectory should be listed under its path");
}

#if ENABLED(CUSTOM_FIRMWARE_UPLOAD)

  /**
   * `M20 F` asks the opposite question, and the answer must be the complement.
   *
   * The same scan classifies every entry as binary or not; `F` selects which class is
   * printed. Listing the same card both ways and asserting each file appears in exactly
   * one of the two replies ties the flag to the extension without depending on how either
   * listing happens to be formatted — and it is the only thing in the suite that observes
   * the binary flag at all. Without `CUSTOM_FIRMWARE_UPLOAD` the flag is compiled out,
   * `setBinFlag()` discards its argument, and the extension test that computes it becomes
   * code that runs with no outcome anything can see.
   */
  MARLIN_TEST(media_listing, M20_F_lists_binary_files_and_only_those) {
    ListingSlate slate;

    put_file("FIRMWARE.BIN", "\x01\x02");
    put_file("SKETCH.GCO", "G28\n");

    const std::string bins = reply_to("M20 F1");
    const std::string all  = reply_to("M20");

    TEST_ASSERT_TRUE_MESSAGE(listed(bins, "FIRMWARE.BIN"), "M20 F should list a .BIN file");
    TEST_ASSERT_FALSE_MESSAGE(listed(bins, "SKETCH.GCO"), "M20 F should not list G-code files");

    TEST_ASSERT_TRUE_MESSAGE(listed(all, "SKETCH.GCO"), "M20 should list a G-code file");
    TEST_ASSERT_FALSE_MESSAGE(listed(all, "FIRMWARE.BIN"), "M20 should not list a .BIN file");
  }

  /**
   * The flag follows the extension one character at a time.
   *
   * Three names that each differ from "BIN" in a single position, none of which may be
   * taken for a firmware file. This is the input class that separates a genuine extension
   * comparison from one that checks only its first character — which is what the surviving
   * mutants on that line were.
   */
  MARLIN_TEST(media_listing, only_a_BIN_extension_counts_as_a_binary_file) {
    ListingSlate slate;

    put_file("REAL.BIN", "\x01");
    put_file("NEARB.BAN", "\x01");
    put_file("NEARB.BIM", "\x01");
    put_file("NEARB.AIN", "\x01");

    const std::string bins = reply_to("M20 F1");

    TEST_ASSERT_TRUE_MESSAGE(listed(bins, "REAL.BIN"), "a .BIN file should be a binary file");
    TEST_ASSERT_FALSE_MESSAGE(listed(bins, "NEARB.BAN"), "a .BAN file is not a binary file");
    TEST_ASSERT_FALSE_MESSAGE(listed(bins, "NEARB.BIM"), "a .BIM file is not a binary file");
    TEST_ASSERT_FALSE_MESSAGE(listed(bins, "NEARB.AIN"), "an .AIN file is not a binary file");
  }

#endif // CUSTOM_FIRMWARE_UPLOAD

/**
 * Selecting by name searches the working directory, and matches the whole name.
 *
 * `M23` scans the current directory comparing DOS names; the comparison is
 * case-insensitive, which is what lets a host send a lower-case name for a card that stores
 * upper-case ones. A prefix is not a match, or `M23 PART` would open `PARTIAL.GCO`.
 */
MARLIN_TEST(media_listing, M23_matches_a_whole_name_regardless_of_case) {
  ListingSlate slate;

  put_file("PARTIAL.GCO", "G28\n");

  send("M23 partial.gco");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "a lower-case name should select the file");
  card.closefile();

  card.selectFileByName("PART");
  send("M23 PART");
  TEST_ASSERT_FALSE_MESSAGE(card.isFileOpen(), "a prefix of a name should not select the file");
}

/**
 * A file opened as a print and a file opened as a macro are announced differently.
 *
 * Both go through the same open, and the only difference a host sees is the word in the
 * notification: a new print is a "fresh" file, a macro or sub-procedure is one the machine is
 * "doing". Hosts key their progress tracking off that distinction, so it is behaviour rather
 * than wording — and it is the only outcome the sub-call type has at this point in the open.
 *
 * Asserting both, on the same file, is what makes it a test of the distinction rather than of
 * either message.
 */
MARLIN_TEST(media_listing, a_print_and_a_macro_are_announced_differently) {
  ListingSlate slate;

  put_file("OPENED.GCO", "G28\n");

  std::string fresh, doing;
  {
    SerialCapture capture;
    card.openFileRead("OPENED.GCO");        // subcall_type 0 — starting a print
    fresh = capture.finish();
  }
  card.closefile();
  {
    SerialCapture capture;
    card.openFileRead("OPENED.GCO", 1);     // subcall_type 1, nothing open — a macro
    doing = capture.finish();
  }
  card.closefile();

  TEST_ASSERT_TRUE_MESSAGE(listed(fresh, "Now fresh file:"), "a new print should be announced as fresh");
  TEST_ASSERT_FALSE_MESSAGE(listed(fresh, "Now doing file:"), "a new print is not a macro");

  TEST_ASSERT_TRUE_MESSAGE(listed(doing, "Now doing file:"), "a macro should be announced as doing");
  TEST_ASSERT_FALSE_MESSAGE(listed(doing, "Now fresh file:"), "a macro is not a new print");
}

#endif // HAS_MEDIA
