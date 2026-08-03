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
  void format() {
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

    // Both FATs start with the media descriptor and an end-of-chain marker; clusters 0
    // and 1 do not exist, so their entries are reserved.
    for (uint8_t f = 0; f < FAT_COUNT; ++f) {
      uint16_t *fat = (uint16_t*)block(RESERVED + f * BLOCKS_PER_FAT);
      fat[0] = 0xFFF8;
      fat[1] = 0xFFFF;
    }
    // The root directory stays zeroed: a first byte of 0x00 means "no more entries".
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
  uint8_t *block(const uint32_t b) { return blocks + size_t(b) * BLOCK_SIZE; }

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
