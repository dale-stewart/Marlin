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
 * Storage, through the firmware's own filesystem.
 *
 * The card is a block of memory (`tests/support/simulated_media.h`) formatted as FAT16 —
 * everything above the blocks is the real `SdVolume`, `SdBaseFile` and `CardReader`. So
 * these are not tests of a fake: they are tests of the FAT implementation, given blocks
 * to work on.
 *
 * The first one matters more than it looks. The whole suite passes with the card
 * unmounted, because nothing else asks; without an assertion that the volume actually
 * mounted, a silently broken image would leave every media test below vacuously true and
 * the configuration would look covered while exercising nothing.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "../test/unit_tests.h"
#include "src/sd/cardreader.h"
#include "../support/simulated_media.h"

#include <string.h>

namespace {

  // The port spins forever on a full transmit buffer when it believes a host is
  // listening, and the media layer reports freely. See CLAUDE.md.
  struct NoHostAttached {
    bool was;
    NoHostAttached() { was = MYSERIAL1.host_connected; MYSERIAL1.host_connected = false; }
    ~NoHostAttached() { MYSERIAL1.host_connected = was; }
  };

  void write_file(const char * const name, const char * const text) {
    card.openFileWrite(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not open the file for writing");
    card.write((void*)text, strlen(text));
    card.closefile();
  }

}

/**
 * The image really is a filesystem the firmware accepts.
 *
 * `SdVolume::init()` decides the FAT width from the cluster count alone, so this also
 * pins the geometry: 8192 blocks, one per cluster, less the 97 used by the boot sector,
 * both FATs and the root directory, leaves 8095 clusters — above the 4085 where the
 * firmware would decide FAT12 (not compiled in) and far below the 65525 where it would
 * decide FAT32.
 */
MARLIN_TEST(media, a_formatted_card_is_mounted) {
  NoHostAttached quiet;
  TEST_ASSERT_TRUE_MESSAGE(card.isMounted(), "the simulated card did not mount");
}

// A file survives being closed and reopened — the directory entry, the FAT chain and the
// data blocks all agree, which is the whole point of writing a real image.
MARLIN_TEST(media, a_file_written_can_be_read_back) {
  NoHostAttached quiet;
  const char * const text = "G28\nG1 X10 Y10\n";

  write_file("round.gco", text);

  card.openFileRead("round.gco");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not reopen the file for reading");
  TEST_ASSERT_EQUAL_UINT32(strlen(text), card.getFileSize());

  char back[32] = { 0 };
  const int16_t got = card.read(back, strlen(text));
  card.closefile();

  TEST_ASSERT_EQUAL_INT16(int16_t(strlen(text)), got);
  TEST_ASSERT_EQUAL_STRING(text, back);
}

/**
 * The firmware puts a file where the specification says it should go.
 *
 * `SdVolume::init()` derives the whole layout from the boot sector — where the FATs start, where
 * the root directory starts, where the data area starts — and until now nothing checked any of
 * it. Those lines run on every mount, so coverage was satisfied; the arithmetic was observed only
 * by whether files happened to read back, which any *self-consistent* set of wrong addresses also
 * satisfies. A firmware that computed the data area two blocks late would write there, read from
 * there, and pass every test in this file.
 *
 * So this asserts the one thing self-consistency cannot fake: that the firmware's addresses agree
 * with an *independent* derivation. `SimulatedMedia` computes the same layout from the FAT
 * specification and the geometry it wrote, and the two are compared at the only place they can
 * be — the actual bytes on the disk.
 *
 * The cluster number is read from the directory entry rather than assumed, so this stays true if
 * an earlier test leaves the first cluster occupied.
 */
MARLIN_TEST(media, a_file_lands_where_the_specification_says_it_should) {
  NoHostAttached quiet;
  const char * const text = "G1 X42 ; a recognisable payload\n";

  write_file("place.gco", text);

  const uint16_t cluster = simulated_card().first_cluster_of("PLACE   GCO");
  TEST_ASSERT_TRUE_MESSAGE(cluster >= 2,
    "the directory entry should name a real data cluster");

  const uint8_t * const where_the_spec_says =
    simulated_card().block(SimulatedMedia::data_block_of_cluster(cluster));

  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(text, where_the_spec_says, strlen(text),
    "the file's bytes should be at the block the FAT geometry puts that cluster at - the "
    "firmware derives that address from the boot sector and the fixture derives it from the "
    "specification, and only their agreement rules out a layout that is wrong but consistent");
}

// A named file is found by name, and only by its own name.
MARLIN_TEST(media, a_written_file_exists_and_an_unwritten_one_does_not) {
  NoHostAttached quiet;

  write_file("present.gco", "M105\n");

  TEST_ASSERT_TRUE(card.fileExists("present.gco"));
  TEST_ASSERT_FALSE(card.fileExists("absent.gco"));
}

// Removing it takes the name out of the directory, so the space is genuinely reclaimed
// rather than the entry merely being skipped.
MARLIN_TEST(media, a_removed_file_stops_existing) {
  NoHostAttached quiet;

  write_file("gone.gco", "M114\n");
  TEST_ASSERT_TRUE(card.fileExists("gone.gco"));

  card.removeFile("gone.gco");
  TEST_ASSERT_FALSE(card.fileExists("gone.gco"));
}

/**
 * Content is addressed by name, not by write order.
 *
 * Two files at once is the first case that needs the FAT chain to be right: with a single
 * file, a broken allocator still returns the only data on the card.
 */
MARLIN_TEST(media, two_files_keep_their_own_contents) {
  NoHostAttached quiet;

  write_file("first.gco", "M104 S200\n");
  write_file("second.gco", "M140 S60\n");

  char back[32] = { 0 };
  card.openFileRead("first.gco");
  card.read(back, 10);
  card.closefile();
  TEST_ASSERT_EQUAL_STRING("M104 S200\n", back);

  memset(back, 0, sizeof(back));
  card.openFileRead("second.gco");
  card.read(back, 9);
  card.closefile();
  TEST_ASSERT_EQUAL_STRING("M140 S60\n", back);
}

#endif // HAS_MEDIA
