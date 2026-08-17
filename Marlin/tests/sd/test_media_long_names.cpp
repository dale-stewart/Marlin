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
 * Reading the names a computer wrote.
 *
 * Every file a printer lists came from somewhere else. `LONG_FILENAME_WRITE_SUPPORT` is off by
 * default, so the firmware writes 8.3 entries and only ever *reads* long ones — which means the
 * long-filename decoder in `SdBaseFile::readDir()` runs against bytes the printer did not
 * produce, from whatever tool the user copied their slicer output with.
 *
 * That decoder was entirely dark. Every media test in this tree names its files in 8.3 —
 * `round.gco`, `present.gco` — and 8.3 names have no VFAT entries at all, so nothing had ever
 * exercised the sequence handling, the checksum, or the orphan detection. The gap was invisible
 * for the usual reason: the file reported 48% and the missing part looked like more of the same.
 *
 * `SimulatedMedia::add_pc_written_file()` is the stand-in for the computer.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "../test/unit_tests.h"
#include "src/sd/cardreader.h"
#include "../support/simulated_media.h"

#include <string.h>
#include <string>

namespace {

  // The port spins forever on a full transmit buffer when it believes a host is listening,
  // and the media layer reports freely. See CLAUDE.md.
  struct NoHostAttached {
    bool was;
    NoHostAttached() { was = MYSERIAL1.host_connected; MYSERIAL1.host_connected = false; }
    ~NoHostAttached() { MYSERIAL1.host_connected = was; }
  };

  /**
   * A card holding exactly what a test put there.
   *
   * Reformatting rather than deleting, because these tests care about *directory order* — a long
   * name is only attached to the entry that follows it — and leftovers from an earlier test
   * would sit in front of this one's entries. Re-mounting afterwards is what makes the volume
   * visible to the firmware again.
   */
  struct CardWrittenByAPC {
    CardWrittenByAPC() { simulated_card().format(); card.mount(); }
    ~CardWrittenByAPC() { simulated_card().format(); card.mount(); }
  };

}

/**
 * A long filename written by a computer is read back whole.
 *
 * The name is what the user recognises; the 8.3 form is what FAT stores, and it is mangled
 * beyond recognition — `benchy.gcode` and `benchy_v2.gcode` both become `BENCHY~1.GCO` and
 * `BENCHY~2.GCO`. A printer that lost the long name would present a menu of files nobody can
 * tell apart, which is why the decoder exists.
 *
 * Twenty-two characters, so it spans two VFAT entries and the join between them is part of what
 * is being asserted: a decoder that read one chunk and stopped, or wrote the second at the wrong
 * offset, produces a plausible-looking truncation rather than an error.
 */
MARLIN_TEST(media_long_names, a_name_written_by_a_computer_is_read_back_whole) {
  NoHostAttached quiet;
  CardWrittenByAPC card_with;

  simulated_card().add_pc_written_file("calibration_cube.gco", "CALIBR~1GCO", "G28\n");
  card.mount();

  card.selectFileByIndex(0);

  TEST_ASSERT_EQUAL_STRING_MESSAGE("calibration_cube.gco", card.longFilename,
    "the long name should survive the trip through two VFAT entries - a decoder that stops "
    "after one chunk truncates rather than fails, and the menu looks merely abbreviated");
}

/**
 * The short name is still there underneath, and it is what the machine acts on.
 *
 * Two names for one file is the part of FAT that surprises people. The printer *shows* the long
 * name and *opens* the short one, so a test that only checked the long name would miss a
 * decoder that had attached it to the wrong entry entirely.
 */
MARLIN_TEST(media_long_names, the_short_name_is_what_the_machine_opens) {
  NoHostAttached quiet;
  CardWrittenByAPC card_with;

  simulated_card().add_pc_written_file("calibration_cube.gco", "CALIBR~1GCO", "G28\nG1 X5\n");
  card.mount();

  card.selectFileByIndex(0);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("CALIBR~1.GCO", card.filename,
    "the 8.3 name should be reported alongside the long one");

  card.openFileRead(card.filename);
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(),
    "and opening it should reach the file the long name was describing");
  TEST_ASSERT_EQUAL_UINT32(10, card.getFileSize());
  card.closefile();
}

/**
 * A long name orphaned by another tool is discarded rather than shown against the wrong file.
 *
 * This is what the checksum in every VFAT entry is for. A tool that does not understand long
 * names — an old card reader, a firmware like this one deleting and rewriting an 8.3 entry —
 * leaves the chunks in place while the entry behind them changes. The chunks then describe a
 * file that is no longer there, and attaching them to whatever now follows would show the user
 * one name and print another.
 *
 * Discarding is the correct answer and it is *silent*, so the assertion is that the long name is
 * empty rather than that anything was reported.
 */
MARLIN_TEST(media_long_names, an_orphaned_long_name_is_discarded) {
  NoHostAttached quiet;
  CardWrittenByAPC card_with;

  simulated_card().add_pc_written_file("wrong_file_name.gco", "RIGHT~1 GCO", "G28\n",
                                       /*orphan_the_long_name=*/true);
  card.mount();

  card.selectFileByIndex(0);

  TEST_ASSERT_EQUAL_STRING_MESSAGE("RIGHT~1.GCO", card.filename,
    "the file itself should still be found");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", card.longFilename,
    "but a long name whose checksum does not match the entry behind it should be dropped - "
    "showing it would name one file while printing another");
}

/**
 * A name longer than this machine's limit is silently truncated, and two files can collide.
 *
 * `longFilename` holds `VFAT_ENTRIES_LIMIT` chunks of thirteen characters plus a terminator — two
 * chunks in most configurations here, five where a DWIN display is fitted — and the reader
 * ignores any chunk numbered past the limit. That limit is not decoration: the next chunk would
 * be written at the offset one past the end of the buffer, so the range check is what stops the
 * overrun.
 *
 * The consequence is what this pins, and it is worse than a truncated string. **Two files whose
 * names differ only after the twenty-sixth character are displayed identically**, which is
 * precisely the situation long filenames exist to prevent — and slicer output is exactly this
 * shape, a common prefix with the variable part at the end. A user picking from the menu cannot
 * tell a draft from a final, and the machine prints whichever the short name happened to attach
 * to.
 *
 * Expected here rather than corrected: the limit is a RAM decision this configuration made, and
 * raising it costs bytes on every board. See the defect register — the fault is that the
 * truncation is silent, not that a limit exists.
 */
MARLIN_TEST(media_long_names, two_names_differing_past_the_limit_are_shown_identically) {
  NoHostAttached quiet;
  CardWrittenByAPC card_with;

  /**
   * Seventy-eight characters each, sharing the first sixty-seven.
   *
   * Long enough to exceed the limit in *every* configuration here, not just this one. The first
   * version of this test hard-coded the twenty-six characters that `004-sd_powerloss` allows and
   * passed there, then failed under `010-dwin` — which enables `HAS_DWIN_E3V2` and with it a
   * limit of five chunks, sixty-five characters, comfortably longer than the name being sent. The
   * expectation below is derived from the configured limit for the same reason.
   */
  const char * const draft = "calibration_cube_0.2mm_PLA_200C_60bed_20mms_infill20_supports_brim_draft.gcode";
  const char * const final_ = "calibration_cube_0.2mm_PLA_200C_60bed_20mms_infill20_supports_brim_final.gcode";
  const std::string kept = std::string(draft).substr(0, VFAT_ENTRIES_LIMIT * FILENAME_LENGTH);

  simulated_card().add_pc_written_file(draft, "CALCUB~1GCO", "G28\n");
  simulated_card().add_pc_written_file(final_, "CALCUB~2GCO", "G28\n");
  card.mount();

  card.selectFileByIndex(0);
  const std::string first_long = card.longFilename, first_short = card.filename;
  card.selectFileByIndex(1);
  const std::string second_long = card.longFilename, second_short = card.filename;

  TEST_ASSERT_EQUAL_STRING_MESSAGE(kept.c_str(), first_long.c_str(),
    "a name past the limit should come back cut to the characters that fit - not dropped, and "
    "with nothing to say it was shortened");

  TEST_ASSERT_TRUE_MESSAGE(first_short != second_short,
    "the two files are distinct on the card");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(first_long.c_str(), second_long.c_str(),
    "and yet they present under the same name - which is the failure worth knowing about, "
    "because a menu of slicer output is a common prefix with the variable part at the end");
}

#endif // HAS_MEDIA
