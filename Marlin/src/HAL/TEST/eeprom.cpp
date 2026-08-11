/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
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
#ifdef __PLAT_TEST__

#include "../../inc/MarlinConfig.h"

#if ENABLED(EEPROM_SETTINGS)

#include "../shared/eeprom_api.h"
#include <string.h>

#ifndef MARLIN_EEPROM_SIZE
  #define MARLIN_EEPROM_SIZE 0x1000 // 4KB of Emulated EEPROM
#endif

/**
 * The store lives in memory and nowhere else.
 *
 * The LINUX HAL backs this with `eeprom.dat` so the simulator keeps its settings between runs,
 * which is a feature there. Here it is a hazard: the file sits in the working directory, is
 * shared by every process that runs from it, and outlives the run. `mutation_test.py` executes
 * its mutant binaries with `cwd=REPO` and up to one worker per core, so a file-backed store puts
 * thirty-odd processes on one file at once — and a test suite that reads state left behind by a
 * previous run is not reproducible even on its own.
 *
 * A test process wants the opposite of persistence: the same starting state every time. So the
 * buffer is process-local and starts erased, which is what a board with never-written EEPROM
 * reads as. Nothing under test can tell the difference — `settings.cpp` sees the same bytes
 * through the same API — and the suite gains isolation between processes for free.
 *
 * See defect register #32.
 */
static uint8_t buffer[MARLIN_EEPROM_SIZE];
static bool erased = false;

constexpr uint8_t EEPROM_ERASE_VALUE = 0xFF;

size_t PersistentStore::capacity() { return MARLIN_EEPROM_SIZE - eeprom_exclude_size; }

bool PersistentStore::access_start() {
  if (!erased) { memset(buffer, EEPROM_ERASE_VALUE, MARLIN_EEPROM_SIZE); erased = true; }
  return true;
}

bool PersistentStore::access_finish() { return true; }

bool PersistentStore::write_data(int &pos, const uint8_t *value, size_t size, uint16_t *crc) {
  std::size_t bytes_written = 0;

  for (std::size_t i = 0; i < size; i++) {
    buffer[pos + i] = value[i];
    bytes_written++;
  }

  crc16(crc, value, size);
  pos += size;
  return (bytes_written != size);  // return true for any error
}

bool PersistentStore::read_data(int &pos, uint8_t *value, const size_t size, uint16_t *crc, const bool writing/*=true*/) {
  std::size_t bytes_read = 0;
  if (writing) {
    for (std::size_t i = 0; i < size; i++) {
      value[i] = buffer[pos + i];
      bytes_read++;
    }
    crc16(crc, value, size);
  }
  else {
    uint8_t temp[size];
    for (std::size_t i = 0; i < size; i++) {
      temp[i] = buffer[pos + i];
      bytes_read++;
    }
    crc16(crc, temp, size);
  }

  pos += size;
  return bytes_read != size;  // return true for any error
}

#endif // EEPROM_SETTINGS
#endif // __PLAT_TEST__
