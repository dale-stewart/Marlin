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
#ifdef __PLAT_LINUX__

#include "../../inc/MarlinConfig.h"

#if ENABLED(EEPROM_SETTINGS)

#include "../shared/eeprom_api.h"
#include <stdio.h>

#ifndef MARLIN_EEPROM_SIZE
  #define MARLIN_EEPROM_SIZE 0x1000 // 4KB of Emulated EEPROM
#endif

uint8_t buffer[MARLIN_EEPROM_SIZE];
char filename[] = "eeprom.dat";

size_t PersistentStore::capacity() { return MARLIN_EEPROM_SIZE - eeprom_exclude_size; }

bool PersistentStore::access_start() {
  constexpr uint8_t eeprom_erase_value = 0xFF;

  // A board whose EEPROM has never been written reads as erased, not as unavailable — so a
  // missing backing file is an empty store, not a failure. Returning false here made the first
  // `M500` of a run silently do nothing on a clean checkout, and the suite pass only from the
  // second run onwards, once an earlier run had left the file behind. See defect register #32.
  FILE * eeprom_file = fopen(filename, "rb");
  if (!eeprom_file) {
    memset(buffer, eeprom_erase_value, MARLIN_EEPROM_SIZE);
    return true;
  }

  fseek(eeprom_file, 0L, SEEK_END);
  const std::size_t file_size = std::size_t(ftell(eeprom_file)),
                    to_read = _MIN(file_size, std::size_t(MARLIN_EEPROM_SIZE));

  // A short file is a partly-written store: read what is there and treat the rest as erased.
  // The previous form memset from `file_size` onward without ever reading the part that existed,
  // so a short file left the low bytes as whatever was in the buffer already.
  fseek(eeprom_file, 0L, SEEK_SET);
  if (to_read) fread(buffer, sizeof(uint8_t), to_read, eeprom_file);
  if (to_read < MARLIN_EEPROM_SIZE)
    memset(buffer + to_read, eeprom_erase_value, MARLIN_EEPROM_SIZE - to_read);

  fclose(eeprom_file);
  return true;
}

bool PersistentStore::access_finish() {
  FILE * eeprom_file = fopen(filename, "wb");
  if (!eeprom_file) return false;
  fwrite(buffer, sizeof(uint8_t), sizeof(buffer), eeprom_file);
  fclose(eeprom_file);
  return true;
}

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
#endif // __PLAT_LINUX__
