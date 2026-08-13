# The test frontier — what may change, and when

Loaded from `legacy-rescue` Step 8.

Cover first, refactor last is easy to state and easy to violate at one remove: editing
untested call sites is refactoring untested code. This file is the boundary rule and what
follows from it.

## Step 8 — Refactor

Only once step 7 passes. The acceptance suite is the safety net; keep it green and
unmodified throughout — if a refactor requires changing a scenario, the refactor
changed behavior.

**The net must not be written in the vocabulary you are about to change.** This is the
concrete reason the acceptance suite comes first, and it is easy to miss: a unit test
that names the API under restructure has to be *edited by the very change it is meant to
be checking*. A test you rewrite as part of a refactor cannot be evidence the refactor
preserved anything — it moved with the code. Count the references before you start; if
the tests you were relying on mention the symbol you are about to remove, they are not
your net, and you need scenarios at a boundary the change does not cross before you can
begin.

The same applies to test fixtures and helpers, not only assertions. Restore state the
way a user would, through the public interface, or teardown becomes another thing the
migration has to edit.

### The test frontier bounds the blast radius

**Refactor freely inside the target. Do not change its public surface until the code
that calls it is itself covered and mutation tested.** Editing hundreds of untested
call sites is refactoring untested code at one remove — the same mistake the whole
workflow exists to prevent, just displaced onto the consumers.

This is what makes cross-cutting corrections — removing global mutable state, undoing
a singleton, replacing a leaky type that appears in a thousand signatures — a
**sequenced migration**, not a refactor:

1. Rescue the target. Refactor its internals behind the existing API.
2. Record the surface change you want as a tracked follow-on, blocked and stated
   plainly, with the reason it is blocked.
3. Rescue each consumer through the same workflow.
4. Only when a consumer is covered may its calls move to the new surface.
5. Change the surface when the last consumer is ready, or introduce the new API
   alongside the old and retire the old as consumers arrive.

**A stub that models an absent device gets code compiling; a simulated device gets it tested.**
Where a dependency is a peripheral or an external service, the cheap shim — nothing attached,
every call fails politely — is the right first move: it makes the code build, and "nothing is
there" is a real state worth pinning, because handling it badly is a common defect. But it only
reaches the error paths. The behaviour the code exists for needs something that answers.

Build the answering version as a *device behind the same seam*, deriving its replies from the
simulated system rather than from recorded traffic, and attach it per test. Two things follow
that are easy to get wrong: give the bus a way to attach devices rather than teaching the bus to
impersonate one, or you encode a single device's protocol into shared infrastructure; and check
what the code caches, because a status field refreshed by one call and read by another will
answer "never seen" to a test that never made the first call — which passes for exactly the
wrong reason when the expected answer is a failure.

**A migration can stall on consumers that cannot be built, not merely untested ones.** The
sequenced migration assumes every consumer can eventually be brought under test. Some cannot:
code that only compiles for one target, that needs hardware or a framework the test environment
has no answer for, or that links only against something the tests would have to invent. Those
consumers are not a coverage gap — no amount of testing reaches them — and treating them as one
leads to writing elaborate fakes for code you still cannot exercise.

When you hit that, the migration is *complete for the reachable code and permanently blocked for
the rest*. Say so, in the header where the surface lives, naming the consumers and the reason.
The alternatives are worse: editing unbuildable code blind is the exact thing the workflow
forbids, and quietly leaving the old surface with no note invites the next person to re-derive
all of it. Making those consumers buildable is a legitimate project — it is simply a different
one, and it has to be sequenced before the migration rather than inside it.

Do not let a blocked surface change become an argument for skipping the cover-first
rule "just this once." The ordering is what makes the correction safe; the correction
is still wanted, and saying so in the follow-on note keeps it from being forgotten.

Work in small, individually reverting commits, in this order:

1. **DRY / L1-L2**: remove duplication, dead code, and misleading names.
2. **SOLID**: single responsibility, then dependency inversion on the seams from
   step 4; extract interfaces where more than one implementation genuinely exists.
3. **Patterns**: apply only where a named force is present. A pattern introduced
   without a problem to solve is added coupling.
4. **Hexagonal architecture**: pure domain at the center; push I/O, frameworks, and
   third-party types out to adapters behind ports the domain owns.

Run the acceptance suite after every commit; run mutation testing at the end to
confirm the net still catches faults in the restructured code.
- **Look for an existing seam before adding one.** Platform or hardware abstraction
  layers, simulation and test builds, public state, configuration switches, and
  dependency-injection points already present for other reasons often provide what a test
  needs. A seam added where one already exists is production risk bought for nothing, and
  it is easy to add without noticing — looking takes minutes. Check any abstraction layer
  the project already has, check what is already public, and check whether the
  collaborator you are trying to fake is even active in the test build: code that never
  runs cannot overwrite what a test writes.
- Only when none exists, do the **minimum refactoring needed to make the code testable**:
  - Break hard dependencies by introducing a seam (parameter, interface, factory,
    or injection point) at the call site.
  - Invert dependencies on I/O, time, randomness, network, filesystem, and hardware
    so a test can substitute a fake.
  - Do not rename, reorganize, or "clean up" anything else at this stage.
  - Each seam is its own commit, separate from test commits.

Exit gate: every survivor is killed, documented as equivalent, or logged as an
open question.

## Quarantining code that cannot be tested

A file whose untestable parts are mixed in with its testable ones reports a coverage figure that
means nothing, and — worse — offers no way to tell "nobody has tested this" from "nobody can".
Both look like a gap, and the second is not one.

Move the unreachable code to its own translation unit and exclude that unit from the coverage
calculation. Entry points the platform calls, loops that never return, bring-up sequences that
touch real hardware in a fixed order: these are unreachable by construction, not by neglect.
Afterwards the figure for what remains means *of the code a test could run*, which is the number
worth acting on.

Three things make this a discipline rather than a dodge:

- **The exclusion list is an argument, not a convenience.** Name each excluded file where the
  build can see it, with the reason beside it. One file with a paragraph explaining why is
  honest; a pattern that quietly swallows a directory is how a coverage figure becomes
  decorative.
- **The quarantined file should only ever get smaller.** It is where work goes to be
  unmeasurable, so anything in it that can be *named and called* belongs back on the testable
  side. Extract-method is the tool: an entry point that reads a register and hands the value to
  a function that decides what it means has moved the decision somewhere assertions can reach,
  and left behind only the read and the ordering.
- **Extract on the seam between "what the hardware says" and "what we do about it."** That
  boundary is where the testable half almost always is, and it is usually straight-line
  reporting or a policy decision sitting in the middle of a bring-up sequence — invisible as a
  candidate until you go looking, because nothing about it looks separable when it is indented
  inside four hundred lines of initialisation.

The payoff is not only the number. Once the boundary is a file rather than a comment, adding to
the untestable side becomes a visible decision that somebody has to justify.
