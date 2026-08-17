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
 * Cards the printer should refuse.
 *
 * `SdVolume::init()` is six rejections in a row, and until now every one of them had run on
 * every test without ever being given anything to reject: this tree formats one good image and
 * mounts it everywhere. That showed up as the largest survivor cluster measured anywhere in the
 * media layer — **151 of 290**, all inside `init()`, on a file at 75% line coverage. Coverage saw
 * the validation execute constantly; mutation saw that deleting it changed nothing.
 *
 * Validators are worth the trouble because they exist for input nobody on this side wrote — a
 * card formatted by a camera, a card pulled out half-way through being written, a card that is
 * not really a card. What is being asserted here is *refusal*: the geometry in a boot sector
 * decides where the firmware will read and write, so accepting a bad one is not a failed mount,
 * it is reads and writes at addresses derived from someone else's arithmetic.
 *
 * Each test damages exactly one field, so each names the single check it reaches.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "../test/unit_tests.h"
#include "../support/simulated_media.h"
#include "src/sd/cardreader.h"

namespace {

  /**
   * A card that is put back in good order however the test ends.
   *
   * This one matters more than most. A malformed image that outlived its test would fail every
   * later mount in the run, and the harness teardown re-mounts rather than reformats — so it
   * would try, fail, and hand the next test a machine with no volume. Reformatting on the way
   * *in* as well as out is what makes a failing test here cost one failure instead of all of
   * them, since the failure path runs no destructor.
   */
  struct GoodCardAfterwards {
    bool was_connected;
    GoodCardAfterwards() {
      was_connected = MYSERIAL1.host_connected;
      MYSERIAL1.host_connected = false;
      restore();
    }
    ~GoodCardAfterwards() { restore(); MYSERIAL1.host_connected = was_connected; }
    static void restore() {
      simulated_card().format();
      card.mount();
    }
  };

  // Lay down a damaged volume and ask the firmware to accept it.
  bool mounts_with(const SimulatedMedia::Malformed damage) {
    simulated_card().format(damage);
    card.mount();
    return card.isMounted();
  }

}

/**
 * The control: the image this tree formats really is accepted.
 *
 * Without it every assertion below is satisfied by a mount that never works, and the whole file
 * would pass against a broken fixture.
 */
MARLIN_TEST(media_bad_volume, a_well_formed_volume_is_accepted) {
  GoodCardAfterwards good;
  TEST_ASSERT_TRUE_MESSAGE(mounts_with(SimulatedMedia::Malformed::None),
    "the undamaged image should mount - if it does not, every refusal below is meaningless");
}

/**
 * A sector size the firmware cannot address is refused.
 *
 * Everything in the FAT layer is arithmetic in 512-byte blocks, and the block device below it
 * transfers exactly that. A volume declaring anything else describes a geometry the firmware
 * has no way to honour, so the only safe answer is to decline it rather than read at offsets
 * that mean something different to the two sides.
 */
MARLIN_TEST(media_bad_volume, a_volume_whose_sectors_are_not_512_bytes_is_refused) {
  GoodCardAfterwards good;
  TEST_ASSERT_FALSE_MESSAGE(mounts_with(SimulatedMedia::Malformed::SectorSizeNot512),
    "a volume with a sector size the block layer cannot transfer should be refused");
}

/**
 * A volume claiming no file allocation tables is refused.
 *
 * The FAT is what turns a file into a chain of clusters. With `fatCount` zero the firmware would
 * compute a root directory starting where the FATs would have ended — that is, at the FATs — and
 * every subsequent address would be wrong by the size of the tables.
 */
MARLIN_TEST(media_bad_volume, a_volume_with_no_allocation_table_is_refused) {
  GoodCardAfterwards good;
  TEST_ASSERT_FALSE_MESSAGE(mounts_with(SimulatedMedia::Malformed::NoFats),
    "a volume with no FAT should be refused, not read as though the tables were zero-sized");
}

/**
 * A volume claiming no reserved sectors is refused.
 *
 * The reserved count is what places the first FAT after the boot sector. Zero would put the FAT
 * on top of the boot sector itself, so the firmware would parse the geometry out of the middle
 * of the allocation table.
 */
MARLIN_TEST(media_bad_volume, a_volume_with_no_reserved_sectors_is_refused) {
  GoodCardAfterwards good;
  TEST_ASSERT_FALSE_MESSAGE(mounts_with(SimulatedMedia::Malformed::NoReservedSectors),
    "a volume that places its first FAT on top of its own boot sector should be refused");
}

/**
 * A cluster size of zero is refused, and so is one that is not a power of two.
 *
 * These look like two checks and are really one. The firmware converts the cluster size into a
 * shift so that multiplying by it is a shift, and discovers a non-power-of-two only by failing
 * to find that shift within eight tries. A size of three is a volume no tool would write and
 * every reader must survive.
 *
 * **The explicit `sectorsPerCluster == 0` test above it is redundant, and that was proved rather
 * than assumed**: removing it fails nothing here, because zero is not a power of two either — the
 * loop compares against `_BV(n)`, never matches, exhausts its eight tries and refuses. So the
 * first assertion below is pinned by the shift loop and not by the check that appears to be for
 * it. Its mutants are equivalent by construction, which is worth knowing before someone spends a
 * round trying to kill them.
 *
 * The redundancy is harmless and is in vendored code, so it is recorded rather than removed.
 */
MARLIN_TEST(media_bad_volume, an_impossible_cluster_size_is_refused) {
  GoodCardAfterwards good;
  TEST_ASSERT_FALSE_MESSAGE(mounts_with(SimulatedMedia::Malformed::NoSectorsPerCluster),
    "a volume with a cluster size of zero should be refused");
  TEST_ASSERT_FALSE_MESSAGE(mounts_with(SimulatedMedia::Malformed::ClusterSizeNotPowerOf2),
    "and so should one whose cluster size is not a power of two - the shift the firmware uses "
    "to multiply by it does not exist");
}

/**
 * A card too small for FAT16 is refused, because this build has no FAT12.
 *
 * The FAT width is decided by the cluster count alone: under 4085 clusters the volume is FAT12,
 * and `FAT12_SUPPORT` is off here. So a genuinely small or unusually formatted card is declined —
 * not because it is corrupt, but because the firmware cannot read that format.
 *
 * Worth pinning as a *behaviour* rather than an accident: it is the reason a working card out of
 * an old device can be rejected by a printer, and the refusal is silent about which of the six
 * reasons applied.
 */
MARLIN_TEST(media_bad_volume, a_card_too_small_for_fat16_is_refused) {
  GoodCardAfterwards good;
  TEST_ASSERT_FALSE_MESSAGE(mounts_with(SimulatedMedia::Malformed::TooFewClustersForFat16),
    "a volume with too few clusters is FAT12, which this build does not compile, so it should "
    "be refused rather than read with the wrong table width");
}

#endif // HAS_MEDIA
