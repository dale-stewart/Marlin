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
 * Writing to a card that has been used before.
 *
 * Every file this suite writes goes onto a volume that was formatted a moment earlier, so the
 * allocator has only ever walked a table of zeros. It takes the first cluster it looks at, every
 * file comes out contiguous, and the branches that exist for a card with history — skipping
 * occupied clusters, wrapping at the end of the table, giving up when there is no room — have
 * never been taken. That is 126 surviving mutants across `allocContiguous`, `fatPut` and
 * `fatGet`, and one missing input class rather than a hundred missing assertions.
 *
 * A real card is the opposite of virgin: it has been written and deleted for months, and a
 * G-code file large enough to matter will not fit in one run of free clusters. The chain is what
 * FAT exists for, and until now nothing had made the firmware build a non-trivial one.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "../test/unit_tests.h"
#include "../support/simulated_media.h"
#include "src/sd/cardreader.h"

#include <string.h>
#include <string>

namespace {

  struct FreshCard {
    bool was_connected;
    FreshCard() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      restore();
    }
    ~FreshCard() { restore(); MYSERIAL1.host_connected = was_connected; }
    // Reformatting on the way in as well as out: a test that fails here leaves a card whose
    // free space is gone, and every later test that writes anything would fail behind it.
    static void restore() { simulated_card().format(); card.mount(); }
  };

  // Longer than one 512-byte cluster, so the allocator has to find more than one.
  std::string a_file_of(const size_t bytes) {
    std::string s;
    while (s.size() < bytes) s += "G1 X" + std::to_string(s.size()) + " ; padding to length\n";
    s.resize(bytes);
    return s;
  }

  void write_file(const char * const name, const std::string &text) {
    card.openFileWrite(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not open the file for writing");
    card.write((void*)text.data(), text.size());
    card.closefile();
  }

  std::string read_file(const char * const name, const size_t bytes) {
    std::string back(bytes, '\0');
    card.openFileRead(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not reopen the file for reading");
    const int16_t got = card.read(&back[0], bytes);
    card.closefile();
    back.resize(got > 0 ? size_t(got) : 0);
    return back;
  }

}

/**
 * A file written across a fragmented card comes back whole.
 *
 * This is what a file allocation table is for, and it had never been exercised: with every
 * cluster free, a multi-cluster file is laid down in one run and reading it back is walking
 * forwards. Occupying alternate clusters forces the firmware to scatter the file and then follow
 * the chain it built.
 *
 * Asserted on the exact bytes rather than the length, because a chain followed wrongly returns
 * the right *number* of bytes taken from the wrong clusters — which is the failure that looks
 * like a corrupt print rather than a failed read.
 */
MARLIN_TEST(media_allocation, a_file_written_across_a_fragmented_card_reads_back_whole) {
  FreshCard fresh;

  // Leave every other cluster in use for the first part of the card, so nothing large fits
  // contiguously and the file has to be chained.
  for (uint16_t c = 2; c < 60; c += 2) simulated_card().occupy_clusters(c, 1);

  const std::string text = a_file_of(4096);          // eight clusters at 512 bytes each
  write_file("frag.gco", text);

  TEST_ASSERT_EQUAL_STRING_MESSAGE(text.c_str(), read_file("frag.gco", text.size()).c_str(),
    "a file scattered across the card should read back byte for byte - a chain followed wrongly "
    "returns the right number of bytes from the wrong clusters, which reads as a corrupt print "
    "rather than as a failed read");
}

/**
 * The allocator does not hand out clusters that are already in use.
 *
 * The check is one line — `if (f != 0) bgnCluster = endCluster + 1;` — and with an empty table it
 * never fires. If it did not fire when it should, the new file would be given clusters belonging
 * to a file already on the card, and *both* would be wrong: the new one interleaved with somebody
 * else's data, the old one silently overwritten.
 *
 * Asserted through the file that was already there, which is the observation that matters. The
 * new file's cluster numbers are an implementation detail; the old file's contents are not.
 */
MARLIN_TEST(media_allocation, allocating_does_not_take_clusters_that_are_in_use) {
  FreshCard fresh;

  const std::string first = a_file_of(2048);
  write_file("first.gco", first);

  const std::string second = a_file_of(2048);
  write_file("second.gco", second);

  TEST_ASSERT_EQUAL_STRING_MESSAGE(first.c_str(), read_file("first.gco", first.size()).c_str(),
    "the file written first should be untouched by the second - an allocator that reused its "
    "clusters would corrupt both, and the older one silently");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(second.c_str(), read_file("second.gco", second.size()).c_str(),
    "and the second should be intact too");
}

/**
 * A card with no room refuses the write rather than pretending.
 *
 * `allocContiguous()` gives up once it has looked at every cluster — `if (n >= clusterCount_)
 * return false` — and that return has to travel all the way back out through `SdBaseFile::write()`
 * as a short count. A firmware that lost it on the way would report a completed upload of a file
 * that is not on the card, which is the worst of the available outcomes: the host deletes its
 * copy.
 *
 * Two clusters are left free so the file starts successfully and runs out part way, which is the
 * case that actually happens; a write refused at the very first cluster would exercise a
 * different and easier path.
 */
MARLIN_TEST(media_allocation, a_full_card_refuses_a_write_that_does_not_fit) {
  FreshCard fresh;

  simulated_card().fill_except(2);                   // room for 1024 bytes, no more

  const std::string text = a_file_of(8192);
  card.openFileWrite("toobig.gco");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "the file should still open - the card is not the "
                                              "problem until the data arrives");
  const int16_t written = card.write((void*)text.data(), text.size());
  card.closefile();

  TEST_ASSERT_TRUE_MESSAGE(written < int16_t(text.size()),
    "a write that runs out of card should report having stored less than it was given - a host "
    "that is told the whole file arrived will delete its own copy");
}

#endif // HAS_MEDIA
