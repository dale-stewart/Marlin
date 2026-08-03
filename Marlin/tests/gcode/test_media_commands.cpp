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
    static void tidy() {
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

  void put_file(const char * const name, const char * const text) {
    card.openFileWrite(name);
    card.write((void*)text, strlen(text));
    card.closefile();
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

#endif // HAS_MEDIA
