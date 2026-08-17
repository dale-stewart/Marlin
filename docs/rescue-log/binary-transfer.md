# Sending a file as bytes: `feature/binary_stream.h`

The framed packet protocol behind `M28 B1` and `CUSTOM_FIRMWARE_UPLOAD`. Opened, not closed.

Part of the rescue log — see [README.md](README.md) for the index and `CLAUDE.md` for the rules
that apply to every session.

## First slice (2026-08-14): 0% -> 30%

Third target from the [survey](survey-2026-08-14.md), and the largest untouched compiled file
left: 100 of 331 countable lines, with config `004-sd_powerloss` going 72.1% -> 72.7%.

**Why it is worth testing at all**, since the survey noted "nothing about it is dangerous" and that
was too generous: ordinary G-code upload is ASCII with a checksum per line, and this replaces it
with framed packets to save the encoding overhead. It is also what `CUSTOM_FIRMWARE_UPLOAD` sends a
*firmware image* over. A corrupted G-code line prints one bad layer; a corrupted firmware image
bricks the board. Every guarantee in the protocol exists to stop the second, and none of them had a
test.

**It tests well for the same reason `e_parser.cpp` did: the protocol publishes its own
vocabulary.** `ss` acknowledges a sync, `ok<n>` acknowledges packet *n*, and the failures are
named — `Packet header(n?) corrupt`, `Packet(n) payload corrupt`, `Datastream packet out of order`.
Those are the words a sender parses, so those are what the assertions compare against, rather than
anything the test computed for itself.

## Speaking the protocol

The tests frame packets byte by byte rather than asking the firmware to do it, which is the point:
a test that had the code under test build its own input would agree with any framing at all.

    token   uint16  0xB5AD, little-endian
    sync    uint8   sequence number
    meta    uint8   protocol in the high nibble, packet type in the low
    size    uint16  payload length
    cksum   uint16  Fletcher-16 over the four bytes above it — *not* the token

**That last line is the detail worth having written down**, because it is not obvious from the
struct and is stated nowhere: the running checksum starts at zero in `PACKET_HEADER`, which the
reader only enters *after* the token has matched, and it is snapshotted at
`bytes_received == sizeof(Header) - 2`. So the token contributes nothing and the checksum field
cannot cover itself. Getting this wrong produces packets the reader rejects, which looks exactly
like a defect in the reader.

Fletcher-16 is written in the test from the algorithm rather than copied from the reader. That is
unavoidable — a test cannot construct a valid packet without speaking the protocol — and it is why
the *assertions* are all on the published responses, where the reader has no say in what the test
expects.

## What the three tests say

- **A sync packet is answered from any state.** `SYNC` is deliberately the one packet whose
  sequence number is *not* checked, because a sender that has lost track cannot guess the right
  one — being answerable from an unknown state is the packet's whole purpose. So it is sent with a
  wildly wrong sync as well as a correct one, which is the case worth asserting rather than an
  aside.
- **A packet with a damaged header is refused, and named.** The header carries the payload length,
  so acting on a corrupted one would read the wrong number of bytes and swallow the packets behind
  it. The reader checks the header alone, before any payload arrives.
- **A packet with a damaged payload is refused, and refused *differently*.** Two checksums guard
  two different things and the reader distinguishes them in what it says: a bad header means it
  does not know how much is coming, a bad payload means it knows and what arrived is wrong. A
  sender uses the difference to decide what to resend, so collapsing the two messages would be a
  protocol change rather than a cosmetic one.

Each corruption flips exactly one checksum rather than sending noise, so a test names the guarantee
it exercises. **Verified by injection, and the useful part is that the two probes are
independent**: forcing the header check true fails only the header test, forcing the payload check
true fails only the payload test. Two tests that both merely detected "something was rejected"
would have failed together.

## Second slice (2026-08-16): the sequence number, and a measurement fault

Five more tests, covering what the first slice left: the acknowledgement, the resend, and the
retry policy. All of it is the *sequence number* rather than the checksums — the second of the two
things the protocol guarantees, and the one whose failures are silent. A checksum failure produces
a rejected packet; a sequence failure produces a file assembled in the wrong order with every
checksum passing.

- **`ok<n>` acknowledges an accepted packet, and the number advances.** Asserted against a `CLOSE`
  packet, so the reply has a *companion*: the reply says accepted, and leaving binary mode says
  acted on. Asserting the reply alone would pass on a reader that acknowledged everything and did
  nothing.
- **A resent packet is acknowledged again and not acted on twice.** The guarantee that makes the
  protocol safe to lose bytes on: a sender cannot distinguish a lost packet from a lost
  acknowledgement, so it resends, and a printer that applied the resend would write the data twice
  — a corrupted upload from a link that dropped nothing. The reader accepts one behind and answers
  without dispatching. Binary mode is switched back on between the two sends, so "not dispatched"
  has something to be observed by.
- **An out-of-sequence packet draws `Datastream packet out of order` and `rs0`** — and the resend
  request names the number the stream *wants*, not the one that arrived, because a sender needs to
  know where to restart.
- **After a resend request, further strays are dropped silently.** Deliberate, and easy to read as
  a bug: a flow-controlled link may have several packets already in flight, and answering each
  would provoke a resend of the whole run. The cost is that a desynchronised stream is silent
  rather than diagnostic, which is why it is pinned.
- **The stream never declares a transfer failed** — see the register entry below.

Verified by injection, four of them, each failing exactly one test and no other:
re-dispatching the resent packet; disabling the in-flight drop; making `max_retries` a real limit;
dropping the sequence number from the acknowledgement.

### Register #60: `max_retries` is 0, and 0 means *no limit*

`static const uint16_t max_retries = 0`, and the guard is
`if (packet_retries < max_retries || max_retries == 0)`. So the zero does not mean "do not retry",
it means "retry for ever" — `PACKET_ERROR`, the `fe` reply and the stream reset behind it are all
unreachable in the shipped firmware. A transfer that cannot resynchronise leaves the printer
asking for a resend indefinitely rather than reporting a failure the host could act on.

Recorded rather than changed, and pinned by `the_stream_never_declares_a_transfer_failed`. The
test drives it with sixteen consecutive **corrupt headers** rather than stray sequence numbers,
because only the checksum path reaches the resend state every time — a stray packet goes quiet
after the first, as above.

### The measurement fault: the test was covering its own copy of the reader

`receive()` is a template on its buffer length, and the first slice's helper declared
`static char line_buffer[256]` because 256 was a comfortable size. The firmware's only call site
passes `MAX_CMD_SIZE`. Those are two instantiations, compiled separately and counted separately,
**so every test in the first slice exercised a copy of the reader that the firmware never
builds.** The assertions were all true of a copy.

It presented as a coverage report that looked broken rather than as a mistake in the test. The
file read 331 lines where it has 240 — the lines counted twice, once per instantiation — and the
uncovered list named the sync-reply lines that the very first test asserts on. The natural
reading is "gcovr is confused"; the correct one is "you measured the other one".

Fixed by taking the length from production rather than choosing it: `char line_buffer[MAX_CMD_SIZE]`,
with the reason written into the helper so the next person does not tidy it back to a round
number. The file then reports its real 240 lines, and the same tests read **52%** instead of 37%.
The whole platform-agnostic tree moved 73.4% -> 74.3% for the same reason — the phantom
instantiation was in the denominator.

The transferable half is in the skill's `scoring.md`: with a generic target the test chooses which
instantiation it measures, and left to itself it will not choose the production one.

## Third slice (2026-08-16): covering the call site, to open the frontier

Step 1 of extracting this header into its `.cpp`, which is what would make it mutation-testable
at all. The rule is that a target's public surface may not move until the code calling it is
covered — and `receive()` has exactly one production caller, `queue.cpp:420`, which the coverage
report listed as **uncovered**. `queue.cpp` is rescued as a file (77.2% raw, ~87% killable) and
that one branch was not reached by anything.

`binary_mode_hands_the_port_to_the_stream_rather_than_the_parser` goes in at
`queue.get_available_commands()` — the entry point the main loop calls, and the only public way
to reach the private `get_serial_commands()`. Every earlier test calls the reader directly, which
proves the reader answers and says nothing about whether the firmware ever gives it the bytes.

It asserts the branch as a **switch** rather than as an effect: text fed in ASCII mode has to
become a queued command, or the binary half would be satisfied by a port nobody was reading. The
bytes differ between the two arms because they must — a packet is not a line of G-code — so what
is held still is the path in, not the input. `queue.cpp` 74% -> 75%, and `420-421` leaves the
missing list.

**The `return` at :421 is an equivalent mutant.** Removing it changes nothing observable: in
binary mode `receive()` drains the port before returning, so the ASCII path it would fall through
to finds an empty buffer. Distinguishing the two would need bytes to arrive between the two
reads. Worth knowing before the eventual mutation run reports it as a survivor.

### The harness fault it turned up (register #61)

The injection that proved the test also failed a test in `test_gcode_acceptance.cpp`, two files
away — a feedrate of 90 where it had asked for 45.

The first diagnosis was the familiar one and was **wrong**: the fixture destructor holding
`card.flag.binary_mode` is indeed skipped by the longjmp, so that reset moved into
`quiesce_simulated_peripherals()` — and the cascade continued unchanged. The mode was not the
leak.

Serial input is staged, and the teardown only knew about the top of it. Bytes land in the port's
receive buffer, are copied one at a time into `queue.serial_state[].line_buffer` with `count`
tracking the current line, and become a queue entry only when a newline arrives. `quiesce` had
cleared the ring buffer and both injection slots for a long time. Nothing reset the accumulator,
so a *partial* command — bytes that arrived but never formed a line — outlived the test that fed
them, and the next test's command was appended to it.

Fixed by resetting `count`, `input_state` and the accumulator for every port, plus draining the
receive buffer. Re-running the same injection now fails exactly one test.

The general form is in the skill's `harness-validation.md`: reset every stage of a staged input,
not just the one with a name — the queue is the obvious one, the half-assembled unit and the raw
transport buffer are the ones that get missed.

## Fourth slice (2026-08-16): the extraction, and the first mutation figure

Steps 2 and 3. `SDFileTransferProtocol`, the two `bs_*` helpers, the heatshrink statics and every
`BinaryStream` member body moved into `binary_stream.cpp`. The header keeps the class declaration,
the nested packet layout, and one line of code.

**The template did not have to go, only the logic behind it.** `receive()` was a template purely
so a caller need not pass a length, and the obvious de-templating —
`receive(char*, size_t)` — trades a compile-time guarantee for a runtime argument nobody checks.
Instead the array form stays in the header as a one-line forward:

    template<const size_t buffer_size>
    void receive(char (&buffer)[buffer_size]) { receive(&buffer[0], buffer_size); }

    void receive(char * const buffer, const size_t buffer_size);   // defined in the .cpp

Every call site keeps its length derived from the buffer it passes, there is now exactly one copy
of the protocol to measure, and **`queue.cpp` did not change at all**. Worth noting against the
step 1 work: the caller had to be covered because the *surface* moved, and in the end the call
site did not — which is the ordering rule doing its job rather than being wasted, since that was
not knowable until the design was settled.

Members that were public only because the class had no access specifier at all are now private:
`checksum()`, `stream_read()`, `dispatch()`, `idle()` and the state fields. Nothing outside ever
used them.

Line coverage is unchanged at 125/240, as it must be — behaviour did not move, only where it is
compiled. The header now reports 1 line.

### 64.9% raw, and where the survivors are

    run 679 · build failures 286 (excluded) · testable 393
    killed 211 · timed out 44 (counted as detected) · survived 138
    score 255/393 = 64.9%   on 125 covered lines, 100s timeout from a 5.0s baseline

Untriaged, so no killable figure yet — 44 timeouts at 11% is high enough to be worth checking
before anyone quotes this.

**The largest survivor cluster is exactly the code that was invisible until now.** Lines 212-231
and 244 — the accessors and the three `reset()` bodies — account for 51 of the 138 survivors,
better than a third. Those were one-line member functions in the header, so before this slice they
were coverage-visible, mutation-invisible, and read as fully exercised.

Triaged in the next slice, where the first reading of that turned out to be wrong.

## Fifth slice (2026-08-16): triage, and a test that was asserting nothing

    testable 394 · killed 241 by assertion · timed out 45 · survived 108
    raw 286/394 = 72.6%   (was 64.9%)
    by assertion 241/394 = 61.2%
    killable 286/346 = 82.7% raw, 241/346 = 69.7% by assertion

Two new tests, and a correction to an old one that mattered more than either.

### The corrupt-payload test was passing for the wrong reason

Writing the missing positive companion — *a packet with an intact payload is accepted* — failed
immediately, and the fault was in the **test builder**, not the reader.

`packet.checksum` is one running Fletcher-16 across the whole packet. The header checksum is a
*snapshot* of it taken two bytes early, which is what lets that field avoid covering itself, but
the running value carries on over the checksum bytes and then over the payload — and that is what
the footer is compared against. The builder computed the footer over the payload alone, so **every
packet it produced had a bad payload checksum**.

The consequence is the point. `a_packet_with_a_damaged_payload_is_refused_and_said_so` asserted
that a corrupted payload draws `payload corrupt` — and it did, but so would an uncorrupted one.
The test discriminated nothing, and it had passed since the first slice, with an injection check
behind it that confirmed only that the *reader's* footer comparison was reachable.

Now verified in both directions: forcing the footer comparison true fails only the corrupt test,
forcing it false fails only the intact test. That is the pair, and neither half is worth much
alone.

Recorded as register #62. The general form — a test that builds input for the code under test can
be wrong in a way that makes a negative assertion vacuous — is in the skill's
`assertion-patterns.md`.

### The other new test

*A stream back in step reports strays again.* The silence after a resend request exists to swallow
packets already in flight, and `packet_retries = 0` in `PACKET_PROCESS` is what ends it. Without
that line the first desynchronisation of a connection would mute every later one for the rest of
the transfer — a worse failure than the noise the silence prevents. Nothing was checking it.

### 45 of the "detections" are the simulated clock standing still

Worth knowing before anyone quotes 72.6%. `receive()` busy-waits: `while (PENDING(millis(),
transfer_window))`, and only `PACKET_WAIT` returns on starvation — the other states `break`, which
re-enters the loop. Under `HAL/TEST` `millis()` moves only when something calls `Clock::advance()`,
and nothing in that loop does, so **any mutant that strands the state machine mid-packet spins for
ever**. Confirmed by building one in directly (the inverted token match at line 273): the suite
runs past ten minutes and is killed.

On real hardware the same mutant does not hang. `millis()` advances, the 20 ms window expires,
`receive()` returns, and the printer carries on with a corrupted transfer. So these are not
detections of the defect — they are detections of the clock. The convention that a timeout counts
as detected is defensible when the hang would also happen in production; here it would not.

That also means the firmware's real behaviour on a stalled packet — *return when the time slice
expires* — is **unreachable in this harness**, because nothing inside the busy-wait advances the
clock. It is the mirror of the problem that retired the LINUX-HAL suite: real time made waiting
untestable, simulated time makes bounded busy-waiting untestable. Both HALs have a blind spot and
this is the one on this side.

### The 44 equivalent mutants, verified rather than argued

Line 214 alone had 20 survivors, and the first reading — "the accessors and resets are what an
assertion-light suite never pins" — was wrong. They are not unasserted; they are **unassertable**,
because every field they clear is unconditionally assigned before it is next read:

- `Header::reset()` clears five fields, and all six header bytes after the token are overwritten
  by the arriving packet. Even `token` is: `PACKET_WAIT` writes `data[1]` and, on a mismatch,
  copies it down to `data[0]`, so a stream that starts `0xAD 0xB5` matches on the second byte
  whatever the reset left behind.
- `Packet::reset()`'s `bytes_received`, `header_checksum` and `buffer` are all assigned on the
  path that reads them. Its `checksum = 0` is *not* — that one is killed, because the running
  checksum genuinely must start at zero.
- `BinaryStream::reset()`'s `buffer_next_index` is set again when a packet is accepted.

Verified by deleting both reset bodies and four of the five fields in `Packet::reset()`
outright: **all 813 tests still pass.** Not a proof of equivalence, but it converts a
line-by-line argument into one experiment.

The firmware is therefore carrying about ten lines of re-initialisation that cannot affect
behaviour. Left alone deliberately — it is defensive rather than wrong, and removing it would make
a future change that reads one of those fields earlier into a silent fault.

**So the corrected lesson from the extraction** is sharper than the first one. What hides in a
header is not merely under-tested; a good part of it is code that no test could pin, because
redundant initialisation is exactly the kind of thing that ends up as a one-liner in a class
declaration. Extracting it is still right — that is how the 44 became *visible* as equivalents
rather than invisible as nothing at all — but expect the honest killable denominator to shrink
rather than the score to rise.

## Sixth slice (2026-08-16): making the busy-wait terminate

    testable 401 · killed 284 by assertion · timed out 9 · survived 108
    raw 293/401 = 73.1%      by assertion 284/401 = 70.8%
    killable 293/357 = 82.1% raw, 284/357 = 79.6% by assertion

**The raw score barely moved and the number is worth far more.** Timeouts went from 47 to 9 and
assertion kills from 248 to 284: thirty-eight mutants that the suite was being credited with
detecting, but which only ever hung the clock, are now genuinely caught by an assertion.

### `HAL_test_set_idle_poll_nanos()`

The blind spot recorded in the fifth slice, closed. Firmware that bounds a loop by elapsed time
rather than by a count has no bound under a clock that moves only on request — nothing inside the
loop moves it. The seam is that such a loop must *poll* something, and a poll that finds nothing
is exactly where a real CPU burns cycles. So `HalSerial::available()` charges an empty poll
against the clock, at a rate a test declares and zero by default.

Opt-in matters: a clock that moves when read would surprise every test that does not want it, and
the default keeps all 814 unaffected. Two properties to know, both in the comment at the
definition — it advances time **without firing timer interrupts**, unlike `HAL_test_advance_micros()`,
and the simulated time a loop consumes depends on how often it polls, so a test should assert
that the loop *ended*, not how long it took.

`FreshStream` switches it on for every test in the file rather than only the one that needs it.
That is what converted the thirty-eight: a well-formed packet never reaches the spin, so nothing
about the passing tests changes, but a mutant stranded mid-packet now returns and is judged on
what it did instead of hanging.

### The test it made possible

*A packet that stops half way times out and asks again.* A sender that dies mid-packet — unplugged,
crashed, a link that dropped — leaves the reader part way through a payload whose length it has
been told. `packet_max_wait` bounds that wait, and nothing was reaching it: lines 390-394 were
uncovered because the state could not be entered. Both messages are asserted, since they say
different things — why the reader gave up, and what it wants the sender to do.

### `BinaryStream::reset()` did not reset the reader

Found the same way as register #61, and by now a familiar shape: an injection that should have
failed one test failed two. `reset()` cleared the sequence number, the retry count and the buffer
index, and left `stream_state` wherever it was — so a stream stranded mid-packet was still
stranded after a "reset", and consumed the front of the next test's bytes as its payload.

Corrected rather than worked around, because the firmware's own one caller already compensated
for it: `PACKET_ERROR` calls `reset()` and then sets `PACKET_RESET` on the next line. Adding that
line to `reset()` changes nothing for the firmware and makes the method mean what it says. With
it, the same injection fails exactly one test.

## Seventh slice (2026-08-16): the file half

Five tests over `SDFileTransferProtocol`, which was the bulk of the file and entirely untested —
open, write, close, abort and query. It needed a card and a file rather than a reply to assert
on, and the card was already there: the harness mounts `SimulatedMedia` for every configuration
with media, so a transfer can be driven end to end and the result read back off the image.

The one that matters is **a file sent as packets arrives on the card byte for byte**. Everything
in the earlier slices asserts what the printer *said*; this asserts what it *kept*. That is the
distinction the whole mode exists for — a transfer that acknowledges every packet and stores the
wrong bytes is indistinguishable on the wire from one that worked, and the failure only appears
when the board is asked to run what it stored. Compared against the exact bytes rather than a
length (which passes on reordered content) or a checksum computed here (which would agree with
whatever convention the writing side used).

The other four are the guards: a write with nothing open is refused rather than handed to
whatever file the card has open; a second transfer is refused *and leaves the first intact*,
which is the part a busy-check in the wrong place would break; an abandoned transfer removes its
partial file, asserted on the card because `PFT:success` is what an abort says either way; and a
query names the version and the compression, because a sender that guessed either would corrupt
the file rather than fail to send it.

Verified by injection: writing one byte fewer, dropping the `transfer_active` guard, and skipping
the `removeFile` in the abort each fail exactly one test and no other.

### `FreshTransfer` cleans up on the way *in*

`transfer_active` is a static inside `binary_stream.cpp` and only a close or an abort clears it,
so a test that fails part way through a transfer leaves the next one answering `PFT:busy` to a
perfectly good open. Neither a destructor nor the harness teardown can reach it — the destructor
because of the longjmp, the teardown because the state is not visible outside that translation
unit.

So the fixture aborts any transfer in progress **in its constructor**, which is the one place the
failure path cannot skip. An abort of a transfer that was not running closes nothing and removes
nothing. This is the "reset by default" shape from the skill, reached for a second reason: not
because a test might forget, but because the state is unreachable from anywhere else.

### Register #63: a finished transfer leaves the card released

Adding these tests failed nine tests in `test_powerloss.cpp` — "no recovery file after save()",
"the record could not be read back". A diagnosis pointing squarely at `powerloss.cpp`, which is
what the failing tests are about and not what was wrong.

`file_close()` and `transfer_abort()` both end with `card.release()`. That is correct for a
printer that has finished with the card, and wrong as the state handed to the next test. The
teardown now re-mounts if a test left it released.

**Third instance of one shape**, and worth naming as a shape rather than three incidents: a test
leaves the machine in a state that is entirely *legitimate* for the firmware — a half-received
line (#61), a state machine stranded mid-packet, a card put away — and the failure surfaces in a
file that has nothing to do with the cause. What they have in common is that none of them is a
flag a test set; each is the ordinary end state of an ordinary operation. The teardown has to be
written from what operations *leave behind*, not from what tests *change*.

### Register #65: the fix for #63 hung the suite, and the diagnosis took three tries

Re-mounting in the teardown made `make unit-test-coverage` hang — for eleven minutes, burning
CPU, at a test that did nothing unusual. The test build stayed green in ten seconds. The same
coverage binary passed six standalone runs: to a file, through a pipe, with stdin closed, and
under a pty.

`CardReader::mount()` announces `SD card ok`. The teardown runs outside any test and therefore
outside `SerialCapture`, which is the only thing that drains the port — and `HalSerial::write()`
busy-waits for room in a 128-byte buffer whenever it believes a host is listening. So the
teardown's own chatter accumulated, a few bytes per test, until the buffer was full and the next
write never returned.

**A slow fuse, not a race**, and that is the whole difficulty: where it burns out depends on how
much everything before it printed, so it moved between builds and between edits and looked
exactly like a timing bug. Two plausible theories came first — unguarded card I/O in the new
tests, then the capture's drainer thread, which has form (#57) — and both were consistent with
the evidence and wrong.

What settled it was not a better theory but a cheaper experiment: comment out the one line the
teardown had gained, and see. Eleven minutes became eleven seconds. The teardown now marks the
port as having no host attached for its own duration and restores what it found.

### Register #64: `ABORT` deletes a file when no transfer is open

Found by writing a fixture, which is its own small lesson. `FreshTransfer` needed to end any
transfer left running, and `ABORT` looked like the idempotent choice. It is not:
`transfer_abort()` is the only one of the four operations with no `transfer_active` guard, so an
abort with nothing open still runs `card.removeFile(card.filename)` — deleting whatever file the
card last touched, which during a print is the job being printed, and answering `PFT:success`.

The fixture uses the guarded `CLOSE` instead. Recorded rather than fixed: it needs a sender to
abort a transfer it never opened, which a correct host does not do — but nothing in the protocol
makes it impossible, and a resend after a lost acknowledgement is exactly the situation where the
two ends disagree about what is open.

### Where it leaves the file

`binary_stream.cpp` **52% -> 86%** (208 of 241 lines), and the platform-agnostic tree 74.5% ->
76.0%.

    testable 558 · killed 378 by assertion · timed out 19 · survived 161
    raw 397/558 = 71.1%      by assertion 378/558 = 67.7%
    killable 77.1% raw, 73.4% by assertion   (43 verified equivalents)
    1009 mutants on 208 covered lines · 105s timeout from a 5.2s baseline
    451 build failures excluded

**The raw score fell from 73.1% and that is not a regression.** The population is a different
one: 208 covered lines against 133, and 558 testable mutants against 401. Covering the file
half brought 157 new mutants into scope, most of them born unkilled, and a score is only
comparable to another over the same population. The previous run's number is not wrong and this
one is not worse — they are answers to different questions, and the difference between them is
not work undone. This is the trap `scoring.md` warns about, met head-on for the first time here.

Timeouts are down to 19 of 397 detections, so the by-assertion figure is now close to the raw
one — the poll-cost change from the sixth slice is what did that, and it holds up on a much
larger population.

## Eighth slice (2026-08-16): four clusters, four tests

Each of the killable clusters above named its missing input, so this round is one test per
cluster. **25 of the 161 survivors killed**, measured by re-running that exact population rather
than by comparing totals — which is the only way to say what a pass achieved. Coverage 86% -> 87%.

- **A transfer whose sender goes quiet is abandoned.** The other end of the abort story: not a
  sender that *says* it is giving up, but one that simply stops. Nothing arrives to trigger
  anything, so the printer notices by itself — `SDFileTransferProtocol::idle()` runs on every pass
  that finds no data, and aborts once the transfer has been quiet past `timeout`. Without it the
  card is held open with a fragment on it and every later upload is refused as busy, until someone
  power-cycles the printer. Testable only because the clock can be moved rather than waited on.
- **An open request with a malformed name is refused.** `Packet::Open::decode()` points at
  `buffer[2]` and hands it on as a C string, and nothing after `validate()` looks for a
  terminator. Two ways to be malformed and both are sent, because a length check alone would pass
  the second: a payload with no room for a name, and a name that never ends.
- **A card that refuses the write reports it.** The one failure a sender cannot detect for itself
  — every other refusal is about the packet, which the sender still holds and can resend, but a
  full card fails *after* the packet arrived intact. Silence there is a host told the file
  transferred when it did not. `PFT:ioerror` rather than `PFT:fail` is the distinction: resending
  will not help.
- **A dummy transfer is accepted and writes nothing.** The flag lets a sender measure the link
  without committing anything to the card, which matters most when what is being sent is a
  firmware image.

### The dummy test did not pin what it claimed, and the injection said so

Worth recording because the test looked obviously right. `a_dummy_transfer_is_accepted_and_writes_nothing`
asserted the transfer succeeded and no file appeared — and removing the dummy guard from
`file_write()` left it **passing**.

Two separate guards read `dummy_transfer`: `file_open()` skips opening the file and `file_write()`
skips writing to it, and only the first is needed for the card to stay clean. So "no file
afterwards" is satisfied by the open guard alone. With the write guard gone, nothing is open,
`card.write()` fails, and there is still no file — the assertion holds for a reason that has
nothing to do with what it names.

What separates them is what the printer *says*: with both guards the write is a silent success,
with only the first it is an I/O error. Asserting that fixed it. The general shape is one already
in `assertion-patterns.md` — an outcome reachable by more than one mechanism pins none of them —
and it is worth noting that it survived being written carefully and was caught only by the
injection.

### The compressed path, and writing an encoder that is not there

The last untested path, and the one where a silent failure is worst. Compression is negotiated
per transfer: the sender says so in the open packet, and every payload afterwards is a heatshrink
stream rather than the file. **A printer that stored those bytes as they arrived would look
identical on the wire** — every packet acknowledged, `PFT:success` at the close, the expected
number of bytes written — and the card would hold a compressed blob. Nothing a sender can see
distinguishes that from a working transfer, which is why the assertion has to be the decompressed
text read back off the card.

Only the *decoder* ships in this firmware, so the test writes the stream from the format instead:
most significant bit first, a `1` tag introducing a literal byte and a `0` introducing a back
reference. Literals only is a legal heatshrink stream that simply saves nothing, and it is all a
test of the decode path needs — what matters is that the printer decompresses, not how well the
sender packed.

The one constraint that is easy to get wrong: nine bits per literal means the input length must
be a multiple of eight for the stream to end on a byte boundary. A partial trailing byte is
padded with zeros, and zero is the tag bit for a back reference, so the decoder would be handed
the start of a token that never arrives. Hence sixteen characters, chosen rather than convenient.

It also drives the flush in `file_close()`, which is where a small transfer's entire output
lives: decoded bytes accumulate in a 512-byte buffer and are written when it fills or when the
transfer closes, and nothing this size ever fills it.

Verified by injection twice, and the failures are legible: storing the stream verbatim gives
"Expected 16 Was 18", dropping the flush gives "Expected 16 Was 0".

**8 more survivors killed, coverage 87% -> 91%**, and the platform-agnostic tree 76.0% -> 77.2% —
the tree moved further than the file because `heatshrink_decoder.cpp` had never been executed by
anything either. It goes 0% -> **52%** (88 of 168 lines) as a side effect, which is a third of
the tree's gain from a single test.

### The back reference, and why the assertion is the text

Done next, and it turned the follow-on note into the most instructive test of the slice.

A literal that decodes wrongly gives a wrong byte. **A back reference that decodes wrongly gives
the wrong run of bytes copied from the wrong place** — still valid-looking text. On a G-code
upload that is a move to somewhere nobody asked for; on a firmware image it is whatever those
bytes happen to mean. It is also where the off-by-ones live: the decoder increments both fields
after reading them (`output_index++`, `output_count++`), so a distance and a length are each
stored one less than they mean.

`HeatshrinkStream` grew a `backref(distance, length)` to go with `literal()`, and the test sends
three literals and one back reference of five bytes from three back — the shape real G-code
compresses into, a repeated line. Five bytes in, eight out.

**The injection made the argument better than the prose does.** Removing the distance increment
gives:

    Expected 'G1\nG1\nG1' Was 'G1\n1\n1\n1'

Same length, different file. A length check passes it; so would a checksum the test computed
itself. Only comparing the exact expected text catches it. Removing the length increment fails on
the size instead, so the two assertions are independent rather than one being a weaker restatement
of the other.

The stream helper now also *asserts* that it ends on a byte boundary rather than leaving it to
the author to remember — padding with zeros would hand the decoder the tag bit of a back
reference that never arrives, and the resulting failure would point at the firmware.

`heatshrink_decoder.cpp` **52% -> 81%**, the tree 77.2% -> 77.9%. `binary_stream.cpp` is
unchanged at 91%, correctly: this test exercises the decoder, not new lines of the protocol.

The decoder has never been mutation tested — it is now the best-covered unmeasured file in the
tree, and a natural next target rather than part of this one. Measured immediately afterwards;
see below.

**And it killed no mutants at all in `binary_stream.cpp`** — 0 of the 128 remaining survivors,
which is the right answer rather than a disappointing one. The test pins behaviour that lives in
`heatshrink_decoder.cpp`; the protocol file's own lines are the same ones the literal test already
exercised. Worth recording because the two numbers disagree in a way that reads as failure: a
test that adds real coverage and real assertions can move the target's mutation score not at all,
because the code it pins is not in the target. The score is about a *file*, and the guarantee is
about a *path through several*.

The transferable half is in `scoring.md` beside the existing note on what the mutation tool can
see — the same distinction from the other side. There, coverage saw code the mutation run could
not reach; here, a test reaches code that belongs to a different measurement altogether.

## Measuring the decoder (2026-08-16): what 81% coverage was worth

`heatshrink_decoder.cpp`, first mutation run ever, driven by the two compression tests above and
nothing else:

    testable 567 · killed 246 by assertion · timed out 28 · survived 293
    raw 274/567 = 48.3%      by assertion 246/567 = 43.4%
    killable 56.1% raw, 50.4% by assertion   (79 structurally unkillable)
    1067 mutants on 137 covered lines · 500 build failures excluded

**81% line coverage, 43% killed by assertion.** That gap is the whole argument for mutation
testing stated in one file: two tests drive the decoder end to end and assert only the final
decoded text, so almost every branch inside the state machine executes without anything checking
what it decided. The coverage figure was never wrong — it just answers a different question.

### A third-party library measured in one configuration is mostly unkillable

The survivors are not evenly spread, and the biggest clusters are not gaps at all:

| Line | Count | Why it cannot be killed |
|---|---|---|
| 291-292 | 39 | `ASSERT(X)` is `#define ASSERT(X) /* no-op */` — debugging logs are off, so the argument is never compiled. Mutating it changes the text of a statement that is not there |
| 214 | 14 | `HEATSHRINK_DECODER_WINDOW_BITS(hsd) > 8`, and this build fixes the window at 8 |
| 252, 274 | 18 | `bit_ct < 8 ? bit_ct : 8` with `bit_ct` fixed at 8 and 4 by the same configuration |
| 341 | 8 | `else if (0)` — dead as written, in the vendored source |

Seventy-nine of 293 survivors, **27%**, are unkillable by construction rather than untested. That
is a much larger share than anything measured in Marlin's own code, and the reason is structural:
a library ships configurable, and a configuration is what makes most of its options dead. Quoting
48.3% for this file without saying so would understate the suite by a quarter and point the next
person at lines no test could ever reach.

The rest are real, and they are the state machine's edges rather than its middle: partial output
when the caller's buffer fills (301-302), input starvation part-way through a token (317, 321),
and the finish states (356). All of them need a transfer shaped differently from the two small
ones here — a payload larger than the output buffer, or a stream split across packets.

## Still to do

The clusters shrank rather than closed: `validate()` 9 -> 5, the idle watchdog 12 -> 8, and the
write arms cleared entirely. What is left there wants either more input classes or triage as
equivalents.

The list as it stood before this slice, for reference:

| Line | Count | What is missing |
|---|---|---|
| 55 | 9 | `Open::validate()` — no malformed OPEN is ever sent. A name with no terminator is exactly the input that makes `filename()` read past the buffer |
| 147-148 | 12 | `SDFileTransferProtocol::idle()`, the watchdog that aborts a transfer left open. Needs a transfer abandoned and the clock advanced past `timeout` |
| 106 | 7 | the `dummy_transfer` arm and the write-failure arm. `simulated_card().fail_writes()` exists for the second |
| 110-115 | 8 | the compressed path and the flush on close |
| 177, 185 | 4 | `PFT:fail` and `PFT:ioerror` |
| 214-231 | 43 | the reset block — verified equivalent, see the fifth slice |

All four of the killable clusters are reachable with fixtures that already exist. None is done.

The `.cpp` also still holds `checksum()`'s arithmetic (3) and the SYNC special-case condition (4)
from earlier slices.

## Not done

`SDFileTransferProtocol` — open, write, close, abort, and the compressed-transfer path — which
needs a card and a file rather than a reply to assert on. That is most of what is left, and it is
a fixture round rather than more tests of the same kind. Also untested: the data-buffer overrun at
line 363, and the timeout path (`Datastream timeout`), which needs simulated time advanced past
`packet_max_wait` mid-packet.

Still no mutation measurement, and there cannot be one: the file is a header, so
`mutation_test.py` cannot reach it. **52% is line coverage and nothing more** — see the header
gotcha in `CLAUDE.md`.
