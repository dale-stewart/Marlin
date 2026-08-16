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

## Not done

`SDFileTransferProtocol` — open, write, close, abort, and the compressed-transfer path — which
needs a card and a file rather than a reply to assert on. That is most of what is left, and it is
a fixture round rather than more tests of the same kind. Also untested: the data-buffer overrun at
line 363, and the timeout path (`Datastream timeout`), which needs simulated time advanced past
`packet_max_wait` mid-packet.

Still no mutation measurement, and there cannot be one: the file is a header, so
`mutation_test.py` cannot reach it. **52% is line coverage and nothing more** — see the header
gotcha in `CLAUDE.md`.
