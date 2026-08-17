# Three files measured at once: what coverage did and did not predict

Part of the rescue log — see [README.md](README.md) for the index and `CLAUDE.md` for the rules
that apply to every session.

Three files were well covered and had **never been mutation tested**: no figure in this log, no
figure anywhere. They were run back to back on 2026-08-17 against `004-sd_powerloss`, one at a
time so the timings stay comparable, and the comparison turned out to be worth more than any of
the three numbers alone.

| file | lines | coverage | raw | by assertion | survivors |
|---|---:|---:|---:|---:|---:|
| `libs/numtostr.cpp` | 270 | 98% | **96.5%** | 94.7% (2085/2201) | 76 |
| `lcd/utf8.cpp` | 81 | 100% | **90.7%** | 77.7% (234/301) | 28 |
| `sd/SdVolume.cpp` | 145 | 75% | **55.1%** | 52.6% (340/646) | 290 |

## The prediction was wrong, and the way it was wrong is the finding

`numtostr.cpp` was picked first because it looked like the most likely embarrassment: 98% covered,
eight open register entries against it, no mutation figure. A high coverage number with nothing
behind it is the exact shape this whole exercise exists to expose.

It came back at **96.5%**, the best figure measured in this tree.

The three files do not differ by how much coverage they have — `utf8.cpp` is at 100% and scores
below `numtostr.cpp` at 98%, and `SdVolume.cpp` at 75% collapses. They differ by **how directly
what the code decides is observed**:

- `numtostr` is pure. A number goes in, a string comes out, and the tests assert the exact string.
  There is nowhere for a wrong decision to hide, and the register entries mean even the strange
  behaviours are pinned.
- `utf8` is a decoder with a little state. Mostly direct, and it sits in the middle.
- `SdVolume` is a stateful FAT allocator and block cache. Nothing observes it directly; everything
  is seen through a file operation several layers up, and most of what it decides never reaches an
  assertion at all.

**So the coverage-to-mutation gap is a property of the code's shape, not of its coverage.** That
refines the rule of thumb this project has been carrying — "expect the mutation score to be far
below the coverage number" — which is true of stateful code and roughly false of pure code with
exact assertions. It is also a scheduling rule: mutation runs are expensive, and they buy the
most on code whose output is observed indirectly.

## `SdVolume`: half the survivors are one missing input

151 of the 290 survivors are in `init()`, and the top lines are all the boot-sector sanity checks —
the partition validity test, `fatCount == 0`, `reservedSectorCount == 0`, and the arithmetic that
derives `dataStartBlock_` from `rootDirEntryCount`.

They survive for one reason: **`SimulatedMedia::format()` lays down exactly one volume, and it is
always valid.** Every test mounts it, so every check runs, and no check has ever been given
something to reject. Coverage sees a validator executing; mutation sees that removing it changes
nothing.

That is the taxonomy's "needs an input, not an assertion" category, and it is cheap to fix — the
fixture already builds the boot sector field by field through the firmware's own `fat_boot_t`, so
producing a malformed one is a parameter, not a new mechanism.

It also matters. A printer handed a corrupt or foreign card has to refuse it rather than act on
whatever geometry it read: `dataStartBlock_` derived from a bad `rootDirEntryCount` is a firmware
reading and writing arbitrary blocks of somebody's card.

The remaining clusters — `allocContiguous` (66), `fatPut` (32), `fatGet` (26) — are the allocator,
and they want the same thing: a card that is nearly full, or fragmented, rather than one with 8095
free clusters and nothing on it.

## Closing the `SdVolume` cluster (2026-08-17)

Seven tests, and the progression is the point:

| | `init()` survivors / testable | killed |
|---|---|---|
| baseline | 151 / 257 | 41.2% |
| + six malformed volumes | 144 / 284 | 49.3% |
| + the geometry cross-check | 139 / 284 | **51.1%** |

### Six refusals, each proved to be the one it names

`format()` gained a `Malformed` parameter naming each rejection — sector size, no FATs, no
reserved sectors, cluster size zero, cluster size not a power of two, too few clusters for FAT16.
One field damaged at a time, after the good image is complete, so a second check cannot fire
first and leave a test pinning a different rejection than it claims.

Verified by disabling each check in turn: five of the six fail **exactly** the test that names
them. The sixth fails nothing — **`sectorsPerCluster == 0` is redundant**, because zero is not a
power of two either, so the shift loop below it already refuses. Its mutants are equivalent by
construction. Harmless, in vendored code, recorded rather than removed — and worth knowing before
someone spends a round trying to kill them.

### The geometry cross-check, and why 840 tests could not see it

The bulk of `init()` is not validation but arithmetic: where the FATs start, where the root
directory starts, where the data area starts. Those lines run on every mount, and they were
observed only by whether files happened to read back — which any *self-consistent* set of wrong
addresses also satisfies.

`a_file_lands_where_the_specification_says_it_should` writes a file, reads the cluster number out
of its directory entry, and asserts the bytes are in the block the **fixture's own** derivation
puts that cluster at. Two independent computations of one address, compared where they can be:
on the disk.

**Shifting `dataStartBlock_` by one block fails exactly one test out of 841.** Every read and
write shifts with it, so the whole suite is blind to a layout that is wrong but consistent. That
injection is the argument for the test; its mutation yield is small (three survivors, plus
fourteen timeouts turned into kills), which is the "justified by injection, not by score" case
already recorded in `scoring.md`.

## Not done

### The allocator: the same treatment, a much poorer return

Three more tests gave the allocator the input it had never had — a fragmented card, a card with
files already on it, a card with no room — through two fixture helpers (`occupy_clusters`,
`fill_except`). All three are verified by injection: making the allocator reuse occupied clusters
or never give up fails them, and making the reader assume files are contiguous fails the
fragmentation test.

And the cluster they were written for barely moved:

| function | before | after |
|---|---|---|
| **`allocContiguous`** | 68/137 | **71/143** |
| `fatPut` | 34/89 | 30/89 |
| `fatGet` | 26/69 | 23/69 |
| `freeChain` | 7/22 | 5/22 |
| file total | 283/673 (57.9%) | 275/679 (**59.5%**) |

Twelve mutants net, and the target cluster flat once its population growth is allowed for.

**The reason is worth more than the tests.** `SdVolume::init()`'s survivors were a missing
*input* — give the validators something to reject and they start deciding. `allocContiguous()`'s
survivors are not. Its remaining mutants are in the search bookkeeping — where the scan starts,
how `bgnCluster` and `endCluster` advance, the wrap at the end of the table — and **any search
that returns a usable run of free clusters satisfies every assertion available from outside**.
The file reads back either way. The choice of clusters is under-determined by the contract.

Killing them would mean asserting *which* clusters were chosen, which is pinning the
implementation — the thing the taxonomy says not to do. So this is the third category rather than
the second: not unasserted, under-determined. The tests are still worth having, because a
fragmented card and a full card are real and were untested; the score was never the thing they
could move.

That is also the honest counter-example to the pattern that has worked all session. A missing
input class explained the validation cluster, the long-filename cluster, the card-detect cluster
and the compression cluster. It does not explain this one, and applying the same move produced a
tenth of the return.
