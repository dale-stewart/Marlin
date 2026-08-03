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
 * A card with a directory tree on it.
 *
 * Every media test written so far put its files in the root, and a mutation run said so:
 * the largest single cluster of surviving mutants in `cardreader.cpp` sat in the loop that
 * walks the working-directory chain, because `workDirDepth` was zero in every test and the
 * loop body never ran. Not one missing assertion — one missing *input class*. These tests
 * supply it.
 *
 * The relationship worth asserting is that the absolute path of a file is the concatenation
 * of the directories entered to reach it, in order, with the file's own DOS name at the end.
 * That is derivable from the sequence of `cd()` calls a test makes, so it can be asserted
 * without knowing what the firmware happens to produce — as opposed to recording one path
 * string and checking the firmware still emits it.
 */

#include "src/inc/MarlinConfig.h"

#if HAS_MEDIA

#include "../test/unit_tests.h"
#include "src/sd/cardreader.h"

#include <string.h>
#include <stdio.h>

namespace {

  // The port spins forever on a full transmit buffer when it believes a host is
  // listening, and the media layer reports freely. See CLAUDE.md.
  struct NoHostAttached {
    bool was;
    NoHostAttached() { was = MYSERIAL1.host_connected; MYSERIAL1.host_connected = false; }
    ~NoHostAttached() { MYSERIAL1.host_connected = was; }
  };

  /**
   * Leave the working directory where the next test expects to find it.
   *
   * `workDir` is global and survives the test that changed it, so a test that dives and
   * fails part-way leaves every later test looking at the wrong directory. As with the
   * heater targets and the write-failure flag, the reset cannot be a destructor: Unity's
   * failure path is a `longjmp` and runs none. It is the first thing each test does.
   */
  void start_at_the_root() { card.cdroot(); }

  // Create a directory chain through the firmware's own FAT writer, so the tests below
  // read back something the firmware itself laid down.
  void make_dir(const char * const path) {
    MediaFile parent = card.getroot(), made;
    made.mkdir(&parent, path);
    made.close();
  }

  void write_file_here(const char * const name, const char * const text) {
    card.openFileWrite(name);
    TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "could not open the file for writing");
    card.write((void*)text, strlen(text));
    card.closefile();
  }

  // The path a test *asked* for, built independently of the firmware: '/', then each
  // directory entered followed by '/', then the file name. Compared against what
  // getAbsFilenameInCWD() reports.
  void expected_path(char *dst, const char * const dirs[], const uint8_t depth, const char * const file) {
    char *p = dst;
    *p++ = '/';
    for (uint8_t i = 0; i < depth; ++i) { strcpy(p, dirs[i]); p += strlen(dirs[i]); *p++ = '/'; }
    if (file) strcpy(p, file); else *p = '\0';
  }

}

/**
 * A directory is entered, and leaving it gets back to where the dive started.
 *
 * `cdup()` is the inverse of `cd()`, which is what makes the chain a stack rather than a
 * path string that happens to be edited at both ends.
 */
MARLIN_TEST(media_directories, a_directory_is_entered_and_left) {
  NoHostAttached quiet;
  start_at_the_root();

  make_dir("MODELS");
  TEST_ASSERT_TRUE_MESSAGE(card.flag.workDirIsRoot, "should start at the root");

  card.cd("MODELS");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.workDirIsRoot, "cd should leave the root");
  TEST_ASSERT_EQUAL_STRING("MODELS", card.getWorkDirName());

  card.cdup();
  TEST_ASSERT_TRUE_MESSAGE(card.flag.workDirIsRoot, "cdup from depth 1 should reach the root");
}

/**
 * The absolute path names every directory on the way down, in order.
 *
 * This is the assertion the survivor cluster was waiting for. It is derived from the dive
 * the test performed, not copied from a previous run's output, so it stays meaningful if
 * the firmware's formatting changes for a reason other than a bug.
 */
MARLIN_TEST(media_directories, the_absolute_path_names_every_directory_on_the_way_down) {
  NoHostAttached quiet;
  start_at_the_root();

  const char * const dirs[] = { "ALPHA", "BETA", "GAMMA" };
  make_dir("ALPHA/BETA/GAMMA");

  for (const char * const d : dirs) card.cd(d);
  write_file_here("DEEP.GCO", "G28\n");

  card.openFileRead("DEEP.GCO");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "the file should open in the current directory");

  char got[MAXPATHNAMELENGTH] = { 0 }, want[MAXPATHNAMELENGTH] = { 0 };
  card.getAbsFilenameInCWD(got);
  card.closefile();

  expected_path(want, dirs, 3, "DEEP.GCO");
  TEST_ASSERT_EQUAL_STRING(want, got);
}

/**
 * One directory less is one path atom less.
 *
 * Alone, the test above is satisfied by any implementation that emits the right string for
 * a depth of three. Asserting that the path shortens by exactly the directory that was left
 * ties the output to the chain rather than to the case.
 */
MARLIN_TEST(media_directories, leaving_a_directory_shortens_the_path_by_that_directory) {
  NoHostAttached quiet;
  start_at_the_root();

  const char * const dirs[] = { "ALPHA", "BETA", "GAMMA" };
  make_dir("ALPHA/BETA/GAMMA");
  for (const char * const d : dirs) card.cd(d);
  write_file_here("DEEP.GCO", "G28\n");

  char deep[MAXPATHNAMELENGTH] = { 0 }, shallow[MAXPATHNAMELENGTH] = { 0 }, want[MAXPATHNAMELENGTH] = { 0 };

  card.openFileRead("DEEP.GCO");
  card.getAbsFilenameInCWD(deep);
  card.closefile();

  card.cdup();
  write_file_here("MID.GCO", "G28\n");
  card.openFileRead("MID.GCO");
  card.getAbsFilenameInCWD(shallow);
  card.closefile();

  expected_path(want, dirs, 2, "MID.GCO");
  TEST_ASSERT_EQUAL_STRING(want, shallow);
  TEST_ASSERT_TRUE_MESSAGE(strlen(shallow) < strlen(deep), "one directory less should be a shorter path");
}

// cdroot() unwinds the whole chain at once, whatever depth it is called from.
MARLIN_TEST(media_directories, cdroot_returns_from_any_depth) {
  NoHostAttached quiet;
  start_at_the_root();

  make_dir("ALPHA/BETA/GAMMA");
  card.cd("ALPHA"); card.cd("BETA"); card.cd("GAMMA");
  TEST_ASSERT_FALSE(card.flag.workDirIsRoot);

  card.cdroot();
  TEST_ASSERT_TRUE_MESSAGE(card.flag.workDirIsRoot, "cdroot should unwind every level");

  write_file_here("TOP.GCO", "G28\n");
  card.openFileRead("TOP.GCO");
  char got[MAXPATHNAMELENGTH] = { 0 };
  card.getAbsFilenameInCWD(got);
  card.closefile();
  TEST_ASSERT_EQUAL_STRING("/TOP.GCO", got);
}

/**
 * A leading slash means the root, wherever the working directory happens to be.
 *
 * This is the one path atom the dive treats specially, and a host sending an absolute path
 * relies on it: a print started from a subdirectory must not resolve `/START.GCO` relative
 * to that subdirectory.
 */
MARLIN_TEST(media_directories, an_absolute_path_ignores_the_working_directory) {
  NoHostAttached quiet;
  start_at_the_root();

  write_file_here("ATROOT.GCO", "M105\n");
  make_dir("ELSEWHR");
  card.cd("ELSEWHR");
  write_file_here("LOCAL.GCO", "M114\n");

  // Relative: found here.
  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("LOCAL.GCO"), "a local file should resolve relative to the cwd");
  // Absolute: found at the root, though the cwd has no such file.
  TEST_ASSERT_TRUE_MESSAGE(card.fileExists("/ATROOT.GCO"), "an absolute path should resolve from the root");
  TEST_ASSERT_FALSE_MESSAGE(card.fileExists("/LOCAL.GCO"), "the subdirectory's file is not at the root");
}

/**
 * A path too long for the buffer drops the filename rather than overflowing it.
 *
 * `getAbsFilenameInCWD` writes into a caller's `MAXPATHNAMELENGTH` buffer, sized as
 * "/" + MAX_DIR_DEPTH * "DIRNAME/" + "filename.ext". At the maximum depth with maximal
 * directory names the directories alone consume the room reserved for the filename, and
 * the function's guard — `cnt < MAXPATHNAMELENGTH - FILENAME_LENGTH - 1` — refuses to append
 * it. That is the correct choice over a buffer overrun, and it is a real limit worth being
 * unable to change silently: the two cases here sit either side of the boundary, so both
 * the guard and the constant it is computed from are pinned.
 *
 * Depth 9 of 8-character names is 1 + 9*9 = 82 characters, under the 91-character limit, so
 * the filename fits. Depth 10 is 91, exactly at it, so it does not.
 */
MARLIN_TEST(media_directories, a_path_that_would_not_fit_stops_at_the_last_directory) {
  NoHostAttached quiet;
  start_at_the_root();

  // Eight characters each, the longest a DOS directory name can be.
  const char * const dirs[MAX_DIR_DEPTH] = {
    "DIRNAM01", "DIRNAM02", "DIRNAM03", "DIRNAM04", "DIRNAM05",
    "DIRNAM06", "DIRNAM07", "DIRNAM08", "DIRNAM09", "DIRNAM10"
  };
  char chain[128] = { 0 };
  for (uint8_t i = 0; i < MAX_DIR_DEPTH; ++i) { if (i) strcat(chain, "/"); strcat(chain, dirs[i]); }
  make_dir(chain);

  // One below the limit: the filename still fits.
  for (uint8_t i = 0; i < MAX_DIR_DEPTH - 1; ++i) card.cd(dirs[i]);
  write_file_here("FITS.GCO", "G28\n");
  card.openFileRead("FITS.GCO");
  char fits[MAXPATHNAMELENGTH] = { 0 }, want[MAXPATHNAMELENGTH] = { 0 };
  card.getAbsFilenameInCWD(fits);
  card.closefile();
  expected_path(want, dirs, MAX_DIR_DEPTH - 1, "FITS.GCO");
  TEST_ASSERT_EQUAL_STRING(want, fits);

  // One deeper: the directories fill the buffer and the filename is left off.
  card.cd(dirs[MAX_DIR_DEPTH - 1]);
  write_file_here("TOOFAR.GCO", "G28\n");
  card.openFileRead("TOOFAR.GCO");
  char full[MAXPATHNAMELENGTH] = { 0 };
  card.getAbsFilenameInCWD(full);
  card.closefile();
  expected_path(want, dirs, MAX_DIR_DEPTH, nullptr);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(want, full, "at the maximum depth the filename should be dropped, not truncated");
  TEST_ASSERT_TRUE_MESSAGE(strlen(full) < MAXPATHNAMELENGTH, "the path must stay inside the buffer it is documented to need");
}

/**
 * The chain stops growing at MAX_DIR_DEPTH.
 *
 * `cd()` still enters the directory — the working directory really does change — but it
 * stops recording parents once the array is full, so the absolute path of anything below
 * that depth is wrong by omission. Recorded rather than corrected: see the defect register.
 */
MARLIN_TEST(media_directories, diving_past_the_maximum_depth_stops_recording_parents) {
  NoHostAttached quiet;
  start_at_the_root();

  const char * const dirs[MAX_DIR_DEPTH + 1] = {
    "DEEPDI01", "DEEPDI02", "DEEPDI03", "DEEPDI04", "DEEPDI05", "DEEPDI06",
    "DEEPDI07", "DEEPDI08", "DEEPDI09", "DEEPDI10", "DEEPDI11"
  };
  char chain[160] = { 0 };
  for (uint8_t i = 0; i <= MAX_DIR_DEPTH; ++i) { if (i) strcat(chain, "/"); strcat(chain, dirs[i]); }
  make_dir(chain);

  for (uint8_t i = 0; i <= MAX_DIR_DEPTH; ++i) card.cd(dirs[i]);

  // The eleventh cd moved the working directory...
  TEST_ASSERT_EQUAL_STRING_MESSAGE(dirs[MAX_DIR_DEPTH], card.getWorkDirName(),
    "cd past the limit should still enter the directory");

  // ...but cdup() unwinds only the ten it recorded, so the eleventh is never left.
  for (uint8_t i = 0; i < MAX_DIR_DEPTH; ++i) card.cdup();
  TEST_ASSERT_TRUE_MESSAGE(card.flag.workDirIsRoot, "ten cdups should exhaust the recorded chain");
}

/**
 * Opening an absolute path moves the working directory to it.
 *
 * The dive that resolves a path is also what maintains the working directory, so a leading
 * slash does two things at once: it restarts the search at the root, and it discards the
 * chain that was there. A print started by absolute path from three directories down must
 * leave the machine in the directory that print came from — otherwise a sub-procedure called
 * by a relative name would be looked for in the wrong place.
 *
 * Asserting the rebuilt path rather than the depth is what makes this specific: a chain that
 * was appended to instead of replaced would have the right length here and the wrong
 * contents.
 */
MARLIN_TEST(media_directories, opening_an_absolute_path_rebuilds_the_working_directory) {
  NoHostAttached quiet;
  start_at_the_root();

  make_dir("OTHER");
  make_dir("ALPHA/BETA");

  card.cd("OTHER");
  write_file_here("TARGET.GCO", "G28\n");
  card.cdroot();

  // Somewhere else entirely, two levels down.
  card.cd("ALPHA"); card.cd("BETA");

  card.openFileRead("/OTHER/TARGET.GCO");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "the absolute path should have opened");

  char got[MAXPATHNAMELENGTH] = { 0 }, want[MAXPATHNAMELENGTH] = { 0 };
  card.getAbsFilenameInCWD(got);
  card.closefile();

  const char * const dirs[] = { "OTHER" };
  expected_path(want, dirs, 1, "TARGET.GCO");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, "the working directory should be the one the path named");
  TEST_ASSERT_FALSE_MESSAGE(card.flag.workDirIsRoot, "one directory down is not the root");
}

// The same dive, ending at the root, says so.
MARLIN_TEST(media_directories, opening_a_path_at_the_root_leaves_the_machine_at_the_root) {
  NoHostAttached quiet;
  start_at_the_root();

  write_file_here("ROOTED.GCO", "G28\n");
  make_dir("ALPHA/BETA");
  card.cd("ALPHA"); card.cd("BETA");
  TEST_ASSERT_FALSE(card.flag.workDirIsRoot);

  card.openFileRead("/ROOTED.GCO");
  TEST_ASSERT_TRUE_MESSAGE(card.isFileOpen(), "the file at the root should have opened");
  TEST_ASSERT_TRUE_MESSAGE(card.flag.workDirIsRoot, "a path with no directories should end at the root");
  card.closefile();
}

#endif // HAS_MEDIA
