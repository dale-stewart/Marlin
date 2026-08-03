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
 * Surviving a power cut mid-print.
 *
 * The feature is a promise about a machine that has been switched off without warning:
 * whatever it knew at the last save must come back after the power does. So the
 * assertions here are about *what survives the round trip* rather than about the layout
 * of the record — the record is written straight to media as a struct, and asserting its
 * bytes would pin the file format instead of the behaviour.
 *
 * The recovery file lives on the simulated card (`tests/support/simulated_media.h`),
 * so the write really goes through the firmware's FAT implementation.
 */

#include "src/inc/MarlinConfig.h"

#if ENABLED(POWER_LOSS_RECOVERY)

#include "../test/unit_tests.h"
#include "../support/simulated_machine.h"
#include "../support/simulated_media.h"

#include "src/feature/powerloss.h"
#include "src/sd/cardreader.h"
#include "src/module/motion.h"
#include "src/module/printcounter.h"
#include "src/module/planner.h"
#include "src/module/temperature.h"
#include "src/gcode/gcode.h"
#include "src/gcode/parser.h"
#include "src/gcode/queue.h"

#include <string.h>

namespace {

  struct NoHostAttached {
    bool was;
    NoHostAttached() { was = MYSERIAL1.host_connected; MYSERIAL1.host_connected = false; }
    ~NoHostAttached() { MYSERIAL1.host_connected = was; }
  };

  // Leave the recovery file and the job timer as they were found: both are process-wide.
  struct CleanSlate {
    NoHostAttached quiet;
    bool was_running;
    CleanSlate() {
      was_running = print_job_timer.isRunning();
      print_job_timer.stop();
      recovery.purge();
    }
    ~CleanSlate() {
      recovery.purge();
      print_job_timer.stop();
      if (was_running) print_job_timer.start();
    }
  };

  /**
   * A record that names a print still on the card.
   *
   * `PrintJobRecovery::valid()` is `info.valid() && interrupted_file_exists()` — the
   * head/foot pair says the record is whole, and the filename says the job it describes
   * can still be found. A record whose file has been deleted is not resumable however
   * intact it is, so a save alone is not enough to make `check()` say yes.
   */
  void save_a_resumable_job(const char * const name = "resume.gco") {
    card.openFileWrite(name);
    card.write((void*)"G28\nG1 Z0.2\n", 11);
    card.closefile();

    // Before the save, not after: `check()` begins by reloading the record from the card,
    // so a filename set only in memory would be overwritten by the one that was stored.
    strncpy(recovery.info.sd_filename, name, sizeof(recovery.info.sd_filename) - 1);
    recovery.info.sd_filename[sizeof(recovery.info.sd_filename) - 1] = '\0';
    recovery.save(true);
  }

  void run_gcode(const char * const line) {
    char buf[64];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    parser.parse(buf);
    gcode.process_parsed_command(true);
  }

}

// Nothing to recover from until something is saved.
MARLIN_TEST(power_loss, no_recovery_file_exists_before_a_save) {
  CleanSlate clean;
  TEST_ASSERT_FALSE(recovery.exists());
}

// A save leaves something for the next boot to find.
MARLIN_TEST(power_loss, saving_leaves_a_recovery_file_behind) {
  CleanSlate clean;

  recovery.save(true);
  TEST_ASSERT_TRUE_MESSAGE(recovery.exists(), "no recovery file after save()");
}

/**
 * The record carries its own integrity check, and it is satisfied.
 *
 * `valid()` is `valid_head && valid_head == valid_foot`, with the two written at
 * opposite ends of the struct. A partial write — the ordinary outcome of losing power
 * *while saving* — leaves them different, so a torn record is rejected rather than
 * resumed from. Asserting the relation rather than the value is the point: the counter
 * increments on every save and the specific number carries no meaning.
 */
MARLIN_TEST(power_loss, a_completed_save_is_marked_valid_at_both_ends) {
  CleanSlate clean;

  recovery.save(true);
  TEST_ASSERT_TRUE(recovery.info.valid());
  TEST_ASSERT_NOT_EQUAL(0, recovery.info.valid_head);
  TEST_ASSERT_EQUAL(recovery.info.valid_head, recovery.info.valid_foot);
}

/**
 * What was saved is what comes back.
 *
 * The state is scribbled over between the write and the read, so a `load()` that did
 * nothing at all could not pass: the values have to come off the card.
 */
MARLIN_TEST(power_loss, the_saved_state_comes_back_after_a_reload) {
  CleanSlate clean;

  motion.position.x = 12.5f;
  motion.position.y = 34.5f;
  motion.position.z = 1.75f;
  recovery.save(true);

  const xyze_pos_t saved = recovery.info.current_position;
  const uint8_t saved_head = recovery.info.valid_head;

  // Obliterate the in-memory copy, so anything read back must come from the card.
  memset(&recovery.info, 0, sizeof(recovery.info));
  TEST_ASSERT_FALSE(recovery.info.valid());

  recovery.load();

  TEST_ASSERT_TRUE_MESSAGE(recovery.info.valid(), "the reloaded record is not valid");
  TEST_ASSERT_EQUAL(saved_head, recovery.info.valid_head);
  TEST_ASSERT_EQUAL_FLOAT(saved.x, recovery.info.current_position.x);
  TEST_ASSERT_EQUAL_FLOAT(saved.y, recovery.info.current_position.y);
  TEST_ASSERT_EQUAL_FLOAT(saved.z, recovery.info.current_position.z);
}

// Finishing normally must not leave a resume offer behind for the next power-on.
MARLIN_TEST(power_loss, purging_removes_the_recovery_file) {
  CleanSlate clean;

  recovery.save(true);
  TEST_ASSERT_TRUE(recovery.exists());

  recovery.purge();
  TEST_ASSERT_FALSE_MESSAGE(recovery.exists(), "the recovery file outlived purge()");
}

/**
 * Everything needed to carry on is in the record, not just where the nozzle was.
 *
 * Resuming reinstates the whole machine, so each of these is separately load-bearing:
 * come back at the wrong feedrate and the print is ruined as surely as coming back in
 * the wrong place. Each value is set to something distinguishable first and read back
 * off the card, so a field that was never copied cannot pass by holding a plausible
 * default.
 */
MARLIN_TEST(power_loss, the_record_carries_the_whole_machine_state) {
  CleanSlate clean;

  motion.feedrate_mm_s = 50.0f;                  // 3000 mm/min
  motion.feedrate_percentage = 90;
  planner.flow_percentage[0] = 115;
  thermalManager.setTargetHotend(205, 0);
  TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(65));
  TERN_(HAS_FAN, thermalManager.set_fan_speed(0, 128));

  recovery.save(true, 3.5f, true);

  // Scribble, so every value below has to have come back off the card.
  memset(&recovery.info, 0, sizeof(recovery.info));
  recovery.load();

  TEST_ASSERT_EQUAL_UINT16(3000, recovery.info.feedrate);          // mm/min, not mm/s
  TEST_ASSERT_EQUAL_INT16(90, recovery.info.feedrate_percentage);
  TEST_ASSERT_EQUAL_INT16(115, recovery.info.flow_percentage[0]);
  TEST_ASSERT_EQUAL_FLOAT(3.5f, recovery.info.zraise);
  TEST_ASSERT_TRUE(recovery.info.flag.raised);

  #if HAS_HOTEND
    TEST_ASSERT_EQUAL_INT16(205, recovery.info.target_temperature[0]);
  #endif
  #if HAS_HEATED_BED
    TEST_ASSERT_EQUAL_INT16(65, recovery.info.target_temperature_bed);
  #endif
  #if HAS_FAN
    TEST_ASSERT_EQUAL_UINT8(128, recovery.info.fan_speed[0]);
  #endif

  thermalManager.setTargetHotend(0, 0);
  TERN_(HAS_HEATED_BED, thermalManager.setTargetBed(0));
}

/**
 * The validity counter is never allowed to land on zero.
 *
 * `if (!++info.valid_head) ++info.valid_head;` exists because zero is the "no record"
 * value — `init()` clears the struct — so a counter that wrapped to zero would mark a
 * perfectly good save as absent. It takes 256 saves to reach, which is why the branch
 * needs an input rather than an assertion: set the counter to its last value and save
 * once more.
 */
MARLIN_TEST(power_loss, the_validity_counter_skips_zero_when_it_wraps) {
  CleanSlate clean;

  recovery.save(true);
  recovery.info.valid_head = 0xFF;
  recovery.info.valid_foot = 0xFF;

  recovery.save(true);   // 0xFF + 1 would be 0, which means "no record"

  TEST_ASSERT_NOT_EQUAL_MESSAGE(0, recovery.info.valid_head, "the counter wrapped to zero");
  TEST_ASSERT_EQUAL(recovery.info.valid_head, recovery.info.valid_foot);
  TEST_ASSERT_TRUE(recovery.info.valid());
}

/**
 * Turning the feature off clears the offer; turning it on while idle does not create one.
 *
 * `changed()` purges when disabled and saves when enabled *and already printing*. The
 * second condition is the interesting one: enabling mid-session on an idle machine must
 * not fabricate a record, or the next boot would offer to resume a print that never ran.
 */
MARLIN_TEST(power_loss, disabling_recovery_removes_the_offer) {
  CleanSlate clean;

  recovery.save(true);
  TEST_ASSERT_TRUE(recovery.exists());

  recovery.enable(false);
  TEST_ASSERT_FALSE_MESSAGE(recovery.exists(), "disabling left the recovery file in place");

  recovery.enable(true);
  TEST_ASSERT_FALSE_MESSAGE(recovery.exists(), "enabling while idle fabricated a record");
}

/**
 * What the machine does when it comes back on.
 *
 * `check()` is the whole feature from the user's side: it runs during startup, and its
 * answer decides whether the printer offers to carry on or quietly forgets. A valid
 * record means an offer — the firmware queues `M1000S`, which is what puts the prompt in
 * front of the operator — and the record is left in place until that resume happens.
 */
MARLIN_TEST(power_loss, a_valid_record_offers_to_resume_at_startup) {
  CleanSlate clean;

  save_a_resumable_job();
  queue.clear();
  queue.injected_commands_P = nullptr;

  TEST_ASSERT_TRUE_MESSAGE(recovery.check(), "a valid record was not offered for resume");
  TEST_ASSERT_TRUE_MESSAGE(recovery.exists(), "the offer was discarded before it was taken up");

  // M1000 is what puts the prompt in front of the operator, so the offer is only real if
  // that command was actually queued. It goes to the injected-command slot, not the ring
  // buffer that host commands arrive in.
  TEST_ASSERT_NOT_NULL_MESSAGE(queue.injected_commands_P, "no resume command was queued");

  queue.injected_commands_P = nullptr;
  queue.clear();
}

/**
 * A torn record is thrown away rather than acted on.
 *
 * Losing power *while saving* is the case the head/foot pair exists for: the two are
 * written at opposite ends of the struct, so a partial write leaves them different. The
 * firmware must treat that as no record at all — resuming from half a record would drive
 * the machine to whatever the uninitialised remainder happened to say. Corrupting the
 * foot and writing is the closest a test can get to power failing mid-write.
 */
MARLIN_TEST(power_loss, a_torn_record_is_discarded_rather_than_resumed) {
  CleanSlate clean;

  save_a_resumable_job();

  // Put a mismatched pair on the card, through the file rather than through save(),
  // which would recompute them. This is what half a write leaves behind.
  job_recovery_info_t torn = recovery.info;
  torn.valid_foot = uint8_t(torn.valid_head + 1);
  card.openFileWrite(recovery.filename);
  TEST_ASSERT_TRUE(card.isFileOpen());
  card.write(&torn, sizeof(torn));
  card.closefile();

  queue.clear();

  TEST_ASSERT_FALSE_MESSAGE(recovery.check(), "a torn record was offered for resume");
  TEST_ASSERT_FALSE_MESSAGE(recovery.exists(), "a torn record was left on the card");
  TEST_ASSERT_NULL_MESSAGE(queue.injected_commands_P, "a resume was queued for a torn record");

  queue.clear();
}

/**
 * A dry run is remembered as one.
 *
 * `M111 S8` makes the printer parse and report without moving or heating. Resuming a dry
 * run as a real print would start extruding onto a bed that was never being printed on,
 * so the flag has to survive with the rest of the state — and it is stored inverted from
 * a bitmask, which is its own opportunity to get the sense backwards.
 */
MARLIN_TEST(power_loss, whether_the_job_was_a_dry_run_survives) {
  CleanSlate clean;
  const uint8_t was = marlin_debug_flags;

  marlin_debug_flags = MARLIN_DEBUG_NONE;
  recovery.save(true);
  TEST_ASSERT_FALSE_MESSAGE(recovery.info.flag.dryrun, "a normal job was recorded as a dry run");

  marlin_debug_flags = MARLIN_DEBUG_DRYRUN;
  recovery.save(true);
  TEST_ASSERT_TRUE_MESSAGE(recovery.info.flag.dryrun, "a dry run was recorded as a normal job");

  marlin_debug_flags = was;
}

/**
 * A note on what cannot be tested here, so it is not re-investigated.
 *
 * `PrintJobRecovery::write()` does `open(false); file.seekSet(0); file.write(...)`, and
 * every mutation of that seek survives — `seekSet(1)`, `seekSet(-1)`, and deleting the
 * call entirely. They are equivalent by construction, not untested: `open(false)` uses
 * `O_TRUNC`, so the file is always zero length at that point, and `SdBaseFile::seekSet()`
 * begins `if (!isOpen() || pos > fileSize_) return false;`. Every target except 0 is past
 * the end and fails, leaving the position where it already was — at 0, which is exactly
 * where `seekSet(0)` would have left it. No assertion can distinguish them.
 *
 * The error branches below the write are `DEBUG_ECHOLNPGM` calls compiled out unless
 * DEBUG_POWER_LOSS_RECOVERY is enabled, so those mutants are the disabled-build-flag
 * class. Between them that accounts for the whole of the write() cluster.
 */

/**
 * A card that has stopped accepting writes does not silently swallow the record.
 *
 * The failure is injected at the block layer, so the firmware's own error handling runs
 * for real: `SdBaseFile::write()` sees the refusal, `PrintJobRecovery::write()` checks
 * the short count, and `close()` fails when the buffered data cannot be flushed. What
 * matters to a user is the outcome — the machine must not come back believing it has a
 * record it does not have.
 */
MARLIN_TEST(power_loss, a_save_to_a_failing_card_does_not_leave_a_usable_record) {
  CleanSlate clean;

  simulated_card().fail_writes();
  recovery.save(true);
  simulated_card().allow_writes();

  // Whatever reached the card, it must not read back as a resumable job.
  memset(&recovery.info, 0, sizeof(recovery.info));
  recovery.load();
  TEST_ASSERT_FALSE_MESSAGE(recovery.valid(),
    "a record written to a failing card was accepted as resumable");
}

/**
 * The record is small enough to land in one block, so at this level it is all or nothing.
 *
 * Worth pinning because it bounds what the head/foot pair can actually defend against.
 * The pair exists to catch a record that was written in part — but the whole struct fits
 * inside a single 512-byte block, so a card refusing writes part-way cannot produce one:
 * either the block lands whole or it does not land at all. A genuinely torn record needs
 * the card to fail *inside* a block, which is a hardware failure mode no block-level
 * interface can express.
 *
 * That is why `a_torn_record_is_discarded_rather_than_resumed` has to corrupt the record
 * by hand: the honest fault injection here cannot reach that state. Asserting both halves
 * of the boundary — one write allowed, none allowed — says which is which rather than
 * leaving the reader to assume the fault injection covered it.
 */
MARLIN_TEST(power_loss, the_record_lands_whole_or_not_at_all) {
  CleanSlate clean;

  TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(SimulatedMedia::BLOCK_SIZE, sizeof(job_recovery_info_t),
    "the record no longer fits in one block - a partial write is now reachable, and this "
    "test should become the torn-record case it was standing in for");

  // One block's worth of writing is the whole record.
  save_a_resumable_job();
  simulated_card().fail_writes(1);
  recovery.save(true);
  simulated_card().allow_writes();

  memset(&recovery.info, 0, sizeof(recovery.info));
  recovery.load();
  TEST_ASSERT_TRUE_MESSAGE(recovery.info.valid(),
    "a record that fitted in the one permitted write did not land whole");
}

// Reads keep working when writes do not — a worn-out card is usually still readable, and
// a fault that broke both would not be testing what it claims to.
MARLIN_TEST(power_loss, a_failing_card_still_reads) {
  CleanSlate clean;

  save_a_resumable_job();
  const uint8_t head = recovery.info.valid_head;

  simulated_card().fail_writes();
  memset(&recovery.info, 0, sizeof(recovery.info));
  recovery.load();
  simulated_card().allow_writes();

  TEST_ASSERT_EQUAL_MESSAGE(head, recovery.info.valid_head, "the record could not be read back");
}

/**
 * The elapsed print time is part of what is saved.
 *
 * Recorded against the timer rather than against a constant: the feature's promise is
 * that the resumed job carries on from where the interrupted one stopped, so the saved
 * value has to be whatever the running timer said, not a number this test chose.
 */
MARLIN_TEST(power_loss, the_elapsed_print_time_is_part_of_the_record) {
  CleanSlate clean;

  print_job_timer.start();
  HAL_test_advance_millis(4000);

  const millis_t running_for = print_job_timer.duration();
  recovery.save(true);

  TEST_ASSERT_EQUAL_UINT32(running_for, recovery.info.print_job_elapsed);
}

/**
 * LEGACY-BEHAVIOR: a resumed job's clock is wrong by the controller's uptime.
 *
 * Defect #4. `PrintJobRecovery::resume()` restores the clock by issuing `M24 T<seconds>`,
 * and `M24` hands that to `Stopwatch::resume()`, which does:
 *
 *     reset();                                     // startTimestamp = 0
 *     if ((accumulator = with_time)) state = RUNNING;
 *
 * `reset()` zeroes `startTimestamp` and nothing sets it again, so `duration()` computes
 * `accumulator + (millis() - 0)` — the saved time plus however long the controller has
 * been powered on.
 *
 * The error is an offset, not a drift, and the difference matters. `millis()` is both the
 * uptime and the thing the elapsed time is measured from, so after the bad start the
 * clock still advances one second per second; it is simply too high, for ever, by the
 * uptime at the moment of the resume. A machine that has been on for an hour before
 * resuming reports the print as an hour older than it is.
 *
 * User-visible as a print that reports hours of elapsed time the moment it resumes, and
 * as a remaining-time estimate computed from it.
 *
 * When this is fixed, `duration()` should equal the resumed time and this test should be
 * inverted to assert that instead.
 */
MARLIN_TEST(power_loss, a_resumed_job_reports_the_uptime_as_well_as_the_resumed_time) {
  CleanSlate clean;

  constexpr millis_t resumed_from_s = 600;   // ten minutes into the interrupted print

  const millis_t uptime_before_s = millis() / 1000;
  run_gcode("M24 T600");
  const millis_t reported_s = print_job_timer.duration();

  // Correct behaviour would be `resumed_from_s`. It is that plus the uptime.
  TEST_ASSERT_UINT32_WITHIN(1, resumed_from_s + uptime_before_s, reported_s);
  TEST_ASSERT_TRUE_MESSAGE(reported_s > resumed_from_s,
    "defect #4 appears to be fixed - invert this test to assert duration() == the resumed time");

  // The rate is right even though the origin is not: ten more seconds of clock adds ten
  // seconds, not twenty. That distinguishes a wrong start from a clock running fast, and
  // it is why the fault is invisible to anyone watching the number climb.
  HAL_test_advance_millis(10000);
  TEST_ASSERT_UINT32_WITHIN(1, reported_s + 10, print_job_timer.duration());
}

#endif // POWER_LOSS_RECOVERY
