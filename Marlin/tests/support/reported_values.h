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
 * Read the numbers back out of what a command reported.
 *
 * `SerialCapture` gives a test the bytes a host would have seen. Most of what the
 * temperature module computes is only visible there — the relay bias of an autotune
 * cycle, the seconds M109 says are left to wait — so a test that wants to assert the
 * arithmetic has to parse the report rather than reach into the firmware for a private
 * local. These are labelled fields in a stream of lines, which is enough structure to
 * read without pretending it is a grammar.
 *
 * Every helper works on a scan position so repeated fields (one report per cycle, one
 * per second) can be walked in order rather than only counted.
 */

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace reported {

  // The number following the next occurrence of `label` at or after `from`.
  // Advances `from` past the label so the next call finds the following one.
  inline bool next_number(const std::string &text, const char * const label, size_t &from, double &out) {
    const size_t at = text.find(label, from);
    if (at == std::string::npos) return false;
    from = at + strlen(label);
    char *end = nullptr;
    out = strtod(text.c_str() + from, &end);
    return end != text.c_str() + from;
  }

  // Every number that follows `label`, in the order they were reported.
  inline std::vector<double> all_numbers(const std::string &text, const char * const label) {
    std::vector<double> found;
    size_t from = 0;
    double v;
    while (next_number(text, label, from, v)) found.push_back(v);
    return found;
  }

  inline size_t occurrences(const std::string &text, const char * const needle) {
    size_t n = 0, from = 0;
    for (;;) {
      const size_t at = text.find(needle, from);
      if (at == std::string::npos) return n;
      n++;
      from = at + strlen(needle);
    }
  }

  inline bool saw(const std::string &text, const char * const needle) {
    return text.find(needle) != std::string::npos;
  }

}
