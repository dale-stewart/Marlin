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
#pragma once

/**
 * A formatted card, in memory.
 *
 * The firmware already abstracts storage at the right level: `DiskIODriver` is a pure
 * virtual interface over 512-byte blocks, and `CardReader::changeMedia()` is public. So a
 * test supplies blocks and lets the firmware's own FAT implementation do the rest —
 * `SdVolume`, `SdBaseFile` and `CardReader` are the code under test, and they are
 * exercised for real rather than stubbed out.
 *
 * The alternative — faking a card at the SPI wire — would mean implementing SD's command
 * protocol in order to test code that sits far above it, and would leave the FAT layer
 * itself untested. See `HAL/TEST/spi.cpp`, which models an empty bus for exactly that
 * reason.
 *
 * The image is built here rather than shipped as a binary fixture, so the geometry is
 * readable and the reason for each number is visible.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "src/sd/disk_io_driver.h"
#include "src/sd/SdFatStructs.h"
#include "src/sd/SdFatConfig.h"   // FILENAME_LENGTH, for the long-name helper below

#include <string.h>
#include <stdlib.h>

class SimulatedMedia : public DiskIODriver {
public:

  /**
   * Geometry, chosen so the firmware decides on FAT16.
   *
   * `SdVolume::init()` picks the FAT width from the cluster count alone: under 4085 it
   * decides FAT12, which this build does not compile in, and at 65525 or more it becomes
   * FAT32. One block per cluster keeps cluster and block numbers the same, which makes
   * the arithmetic below checkable by eye.
   *
   *   reserved   1 block   the boot sector itself
   *   FATs       2 x 32    32 blocks holds 8192 16-bit entries, one per cluster
   *   root dir   32        512 entries at 32 bytes each
   *   data       the rest  8192 - 97 = 8095 clusters, comfortably inside the FAT16 range
   */
  static constexpr uint32_t BLOCK_SIZE     = 512;
  static constexpr uint32_t TOTAL_BLOCKS   = 8192;          // 4 MB
  static constexpr uint16_t RESERVED       = 1;
  static constexpr uint8_t  FAT_COUNT      = 2;
  static constexpr uint16_t BLOCKS_PER_FAT = 32;
  static constexpr uint16_t ROOT_ENTRIES   = 512;
  static constexpr uint8_t  BLOCKS_PER_CLUSTER = 1;

  SimulatedMedia() { blocks = (uint8_t*)calloc(TOTAL_BLOCKS, BLOCK_SIZE); format(); }
  ~SimulatedMedia() { free(blocks); }

  /**
   * Lay down an empty FAT16 volume.
   *
   * Written through the firmware's own `fat_boot_t` rather than at raw byte offsets, so
   * the fields cannot drift apart from the ones `SdVolume::init()` reads back.
   *
   * No master boot record: `SdVolume::init(dev)` tries partition 1 first and falls back
   * to treating block zero as the boot sector, and the partition attempt fails harmlessly
   * because the bytes where a partition table would sit are zero.
   */
  /**
   * The ways a volume can be refused, each named for the check that refuses it.
   *
   * `SdVolume::init()` is six rejections in a row and a positive control had been the only
   * input any of them ever saw — every test in this tree mounts one perfectly good image, so
   * the validation ran constantly and decided nothing. A validator that has never been given
   * something to reject is a validator nobody has tested, and validators exist precisely for
   * input you did not write: a card formatted by another device, a card half-written, a card
   * that is not a card.
   */
  enum class Malformed : uint8_t {
    None,
    SectorSizeNot512,        // bytesPerSector != 512
    NoFats,                  // fatCount == 0
    NoReservedSectors,       // reservedSectorCount == 0
    NoSectorsPerCluster,     // sectorsPerCluster == 0
    ClusterSizeNotPowerOf2,  // the shift loop gives up past 7
    TooFewClustersForFat16   // cluster count below 4085 means FAT12, which this build omits
  };

  void format(const Malformed damage = Malformed::None) {
    memset(blocks, 0, size_t(TOTAL_BLOCKS) * BLOCK_SIZE);

    fat_boot_t *fbs = (fat_boot_t*)block(0);
    fbs->jump[0] = 0xEB; fbs->jump[1] = 0x3C; fbs->jump[2] = 0x90;
    fbs->bytesPerSector      = BLOCK_SIZE;
    fbs->sectorsPerCluster   = BLOCKS_PER_CLUSTER;
    fbs->reservedSectorCount = RESERVED;
    fbs->fatCount            = FAT_COUNT;
    fbs->rootDirEntryCount   = ROOT_ENTRIES;
    fbs->totalSectors16      = TOTAL_BLOCKS;
    fbs->mediaType           = 0xF8;                  // fixed disk
    fbs->sectorsPerFat16     = BLOCKS_PER_FAT;
    fbs->sectorsPerTrack     = 32;
    fbs->headCount           = 8;
    fbs->hidddenSectors      = 0;
    fbs->totalSectors32      = 0;                     // 0 when totalSectors16 is used
    fbs->driveNumber         = 0x80;
    fbs->bootSignature       = 0x29;
    fbs->volumeSerialNumber  = 0x4D41524C;            // "MARL"
    fbs->bootSectorSig0      = 0x55;
    fbs->bootSectorSig1      = 0xAA;

    /**
     * Break exactly one field, after the good image is complete.
     *
     * One at a time and last, so that each test names the single check it reaches. Damaging a
     * field while building would risk a second check firing first, and the test would pass
     * while pinning a different rejection than the one it claims.
     */
    switch (damage) {
      case Malformed::None: break;
      case Malformed::SectorSizeNot512:       fbs->bytesPerSector      = 1024; break;
      case Malformed::NoFats:                 fbs->fatCount            = 0;    break;
      case Malformed::NoReservedSectors:      fbs->reservedSectorCount = 0;    break;
      case Malformed::NoSectorsPerCluster:    fbs->sectorsPerCluster   = 0;    break;
      case Malformed::ClusterSizeNotPowerOf2: fbs->sectorsPerCluster   = 3;    break;
      // Few enough clusters that the firmware decides FAT12, which it does not compile in.
      // A small or oddly formatted card really does present this way.
      case Malformed::TooFewClustersForFat16: fbs->totalSectors16      = 200;  break;
    }

    // Both FATs start with the media descriptor and an end-of-chain marker; clusters 0
    // and 1 do not exist, so their entries are reserved.
    for (uint8_t f = 0; f < FAT_COUNT; ++f) {
      uint16_t *fat = (uint16_t*)block(RESERVED + f * BLOCKS_PER_FAT);
      fat[0] = 0xFFF8;
      fat[1] = 0xFFFF;
    }
    // The root directory stays zeroed: a first byte of 0x00 means "no more entries".
  }

  // ---- Content this firmware cannot create for itself ----

  /**
   * Put a file on the card the way a PC does, with a long filename.
   *
   * This exists because the firmware can *read* long filenames and cannot *write* them —
   * `LONG_FILENAME_WRITE_SUPPORT` is off by default, so `openFileWrite()` lays down an 8.3
   * entry and nothing else. Every long name a printer ever sees was therefore written by
   * something other than the printer, which is exactly what this stands in for: the user
   * copying `.gcode` files onto the card from their computer.
   *
   * A long name is stored as VFAT entries placed *before* the 8.3 entry, in reverse order —
   * the highest sequence number first, flagged 0x40 as the last chunk, counting down to 1,
   * then the short entry itself. Each entry carries thirteen UTF-16 characters and a
   * checksum of the 8.3 name, which is how a reader detects entries orphaned by a tool that
   * did not understand them.
   *
   * `short_name` is the raw eleven-byte directory form — space-padded, no dot, as it sits on
   * the disk (`"BENCHY~1GCO"`). Taking it in that form rather than deriving it keeps the
   * 8.3 mangling rules out of a fixture whose subject is the long name.
   *
   * The checksum is computed here from the FAT specification rather than by calling the
   * firmware's `lfn_checksum()`. That is deliberate and is the same reasoning as writing
   * Fletcher-16 by hand in the binary-transfer tests: a fixture that asks the code under
   * test to prepare its own input agrees with that code however wrong it is.
   */
  void add_pc_written_file(const char * const long_name, const char * const short_name,
                           const char * const contents = "",
                           const bool orphan_the_long_name = false) {
    const size_t name_len = strlen(long_name);
    const uint8_t chunks = uint8_t((name_len + FILENAME_LENGTH - 1) / FILENAME_LENGTH);

    // Checksum of the 8.3 name, per the FAT long-filename specification: rotate right and add.
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < 11; ++i)
      checksum = uint8_t(((checksum & 1) << 7) + (checksum >> 1) + uint8_t(short_name[i]));

    // An orphan is what a tool that does not understand long names leaves behind when it
    // renames or replaces the 8.3 entry: the chunks stay, and their checksum no longer
    // describes the file they sit in front of.
    if (orphan_the_long_name) checksum = uint8_t(checksum ^ 0xFF);

    dir_t * const dir = root_entries();
    uint16_t slot = 0;
    while (slot < ROOT_ENTRIES && dir[slot].name[0] != DIR_NAME_FREE) slot++;

    // The chunks go down from the last, which is the order a reader walking forward expects.
    for (uint8_t seq = chunks; seq >= 1; --seq) {
      vfat_t &v = *(vfat_t*)&dir[slot++];
      memset(&v, 0, sizeof(v));
      v.sequenceNumber = uint8_t(seq | (seq == chunks ? 0x40 : 0x00));
      v.attributes = DIR_ATT_LONG_NAME;
      v.checksum = checksum;
      v.firstClusterLow = 0;                        // always zero for a long-name entry
      for (uint8_t i = 0; i < FILENAME_LENGTH; ++i) {
        const size_t at = size_t(seq - 1) * FILENAME_LENGTH + i;
        // Past the end of the name: one NUL, then 0xFFFF padding, as the specification says.
        const uint16_t ch = at < name_len ? uint16_t(uint8_t(long_name[at]))
                                          : (at == name_len ? 0x0000 : 0xFFFF);
        if (i < 5)       v.name1[i] = ch;
        else if (i < 11) v.name2[i - 5] = ch;
        else             v.name3[i - 11] = ch;
      }
    }

    dir_t &e = dir[slot];
    memset(&e, 0, sizeof(e));
    memcpy(e.name, short_name, 11);
    e.attributes = DIR_ATT_ARCHIVE;
    e.fileSize = uint32_t(strlen(contents));
    e.firstClusterLow = e.fileSize ? store_in_one_cluster(contents) : 0;
  }

  // ---- Fault injection ----

  /**
   * Start refusing writes, the way a card does once it has been pulled out, worn out, or
   * write-protected.
   *
   * Failure is reported at the block layer — `writeBlock()` returns false — because that
   * is where a real card refuses, and it lets the firmware's own error handling run:
   * `SdBaseFile::write()` turns it into a short count, `PrintJobRecovery::write()` checks
   * for -1, and `close()` fails when the buffered data cannot be flushed. Faking a
   * failure higher up would skip the code the failure exists to exercise.
   *
   * `after` is how many writes still succeed first, so a card can be made to die
   * part-way through a record rather than only before it — which is the interesting case
   * for anything that writes more than one block.
   *
   * Deliberately not RAII. A failing assertion leaves a test through a `longjmp`, which
   * runs no destructor, so a scope guard would leak the fault into every later test. The
   * harness clears it after each test instead — see `quiesce_simulated_peripherals()`.
   */
  void fail_writes(const uint32_t after = 0) { writes_allowed = after; failing = true; }
  void allow_writes() { failing = false; writes_allowed = 0; }
  bool writes_are_failing() const { return failing; }

  // ---- DiskIODriver ----

  bool init(const uint8_t, const pin_t) override { return blocks != nullptr; }

  bool readCSD(csd_t * const) override { return false; }   // size comes from cardSize()

  bool readBlock(const uint32_t b, uint8_t * const dst) override {
    if (b >= TOTAL_BLOCKS) return false;
    memcpy(dst, block(b), BLOCK_SIZE);
    return true;
  }

  bool writeBlock(const uint32_t b, const uint8_t * const src) override {
    if (b >= TOTAL_BLOCKS) return false;
    if (failing) {
      if (writes_allowed == 0) return false;   // the card refuses from here on
      --writes_allowed;
    }
    memcpy(block(b), src, BLOCK_SIZE);
    return true;
  }

  // Multi-block transfers, which the FAT layer uses for runs of clusters.
  bool readStart(const uint32_t b) override { cursor = b; return b < TOTAL_BLOCKS; }
  bool readData(uint8_t * const dst) override { return readBlock(cursor++, dst); }
  bool readStop() override { return true; }

  bool writeStart(const uint32_t b, const uint32_t) override { cursor = b; return b < TOTAL_BLOCKS; }
  bool writeData(const uint8_t *src) override { return writeBlock(cursor++, src); }
  bool writeStop() override { return true; }

  uint32_t cardSize() override { return TOTAL_BLOCKS; }
  bool isReady() override { return blocks != nullptr; }
  void idle() override {}

private:
  // The layout the geometry above implies: boot sector, both FATs, then the root directory,
  // then the data area. Cluster numbering starts at 2, so cluster 2 is the first data block.
  static constexpr uint32_t ROOT_START = RESERVED + FAT_COUNT * BLOCKS_PER_FAT;
  static constexpr uint32_t DATA_START = ROOT_START + ROOT_ENTRIES * 32 / BLOCK_SIZE;

  dir_t *root_entries() { return (dir_t*)block(ROOT_START); }

public:
  /**
   * The image's own view of where things are, derived here from the FAT specification.
   *
   * Public so a test can cross-check it against the firmware's, which computes the same
   * addresses from the boot sector it reads back. Two independent derivations of one quantity
   * is a real assertion; asking the firmware where it put something and then looking there is
   * not.
   */
  uint8_t *block(const uint32_t b) { return blocks + size_t(b) * BLOCK_SIZE; }
  static constexpr uint32_t data_block_of_cluster(const uint16_t cluster) {
    return DATA_START + (cluster - 2) * BLOCKS_PER_CLUSTER;
  }
  uint16_t first_cluster_of(const char * const short_name) {
    dir_t * const dir = (dir_t*)block(ROOT_START);
    for (uint16_t i = 0; i < ROOT_ENTRIES; ++i)
      if (!memcmp(dir[i].name, short_name, 11)) return dir[i].firstClusterLow;
    return 0;
  }

private:

  // Write short contents into the first free cluster and close its chain. One cluster is
  // enough for anything a directory test needs, and the assertion says so rather than
  // silently truncating.
  uint16_t store_in_one_cluster(const char * const contents) {
    const size_t len = strlen(contents);
    TEST_ASSERT_TRUE_MESSAGE(len <= BLOCK_SIZE,
      "the simulated card's helper stores one cluster; a longer file needs a chain");
    uint16_t cluster = 2;
    uint16_t *fat = (uint16_t*)block(RESERVED);
    while (cluster < 0xFFF0 && fat[cluster] != 0) cluster++;
    for (uint8_t f = 0; f < FAT_COUNT; ++f)
      ((uint16_t*)block(RESERVED + f * BLOCKS_PER_FAT))[cluster] = 0xFFFF;   // end of chain
    memcpy(block(DATA_START + (cluster - 2) * BLOCKS_PER_CLUSTER), contents, len);
    return cluster;
  }

  uint8_t *blocks = nullptr;
  uint32_t cursor = 0;
  bool failing = false;         // refusing writes
  uint32_t writes_allowed = 0;  // ...after this many more have succeeded
};

/**
 * The one card the harness installs, and the one tests reach for to make it misbehave.
 *
 * Constructed on first use rather than as a namespace-scope object: it is referenced from
 * both the harness and individual tests, and a static with a constructor would reintroduce
 * exactly the initialisation-order dependency that register entry #20 was about.
 */
inline SimulatedMedia& simulated_card() {
  static SimulatedMedia the_card;
  return the_card;
}

#endif // HAS_MEDIA
