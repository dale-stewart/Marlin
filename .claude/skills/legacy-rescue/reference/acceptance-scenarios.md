# Acceptance scenarios — writing them, and validating them alone

Loaded from `legacy-rescue` Steps 6 and 7.

## Step 6 — Extract Gherkin scenarios and build acceptance tests

Now that behavior is pinned, describe it in the domain's language.

**Scenarios belong to user-facing features, not to modules.** Write them at the system
boundary, about outcomes a user or client would recognise. Do **not** create a feature
file per rescued file: a leaf utility — a formatter, a parser helper, a maths routine —
earns no feature of its own and is exercised *through* the feature that uses it. If the
only way to describe a scenario is in terms of the module you just rescued, you are
writing a unit test in Gherkin syntax; keep it as a unit test.

**There is no one-to-one correspondence between scenarios and unit tests, and there
should not be.** The two suites answer different questions and are sized differently:

| | Unit tests | Scenarios |
|---|---|---|
| Pin | boundaries, edge cases, error paths, exact formats | outcomes a user would notice |
| Number | many per module | few per feature |
| Read by | whoever changes that module | anyone deciding what the system does |

What they owe jointly is **coverage of all the code, directly or indirectly**. A line
reached only through a scenario is covered; a line reached only by a unit test is
covered. Neither suite has to reach everything on its own, but between them nothing
should be left unexercised — that union is what step 7 measures.

**Write the outcome, not the mechanism.** The scenario text says what the user gets;
the step definitions do the translating. If a step reads like a function call with the
parentheses removed, push the detail down into the step.

    # Mechanism — belongs in a step definition, not in the feature
    When formatAmount is called with 12.345
    Then the result is "  12.35"

    # Outcome — what a user would actually observe
    When the balance reaches 12.345
    Then the statement shows the amount to two decimal places

- Use the domain's vocabulary. If a scenario cannot be written without naming a private
  class, it belongs at a lower level.
- Implement scenarios against the public surface with reusable steps; a step is the
  single place where domain language becomes an API call.
- Any `LEGACY-BEHAVIOR:` marker from step 1 that survives to here is a question for
  the user: intended behavior or long-lived bug?

Exit gate: scenarios reviewed by the user; acceptance suite green.

## Step 7 — Validate the acceptance suite on its own

Re-run coverage **and** mutation testing with the unit tests excluded, so only the
Gherkin-based suite is exercised.

This is the load-bearing check for anything you intend to restructure in step 8: it
shows how much of the behavior the scenarios describe by themselves, which is what
makes a refactor safe. **Measure the rescued code's coverage under the acceptance suite
even when no scenario mentions it** — a utility reached indirectly through a feature is
still protected by that feature, and that indirect reach is exactly what you need to
know before restructuring it.

Read the result by what it protects, not against a fixed number:

- **A gap in behavior a user would notice** is a missing scenario. Add it; do not
  backfill with a unit test.
- **A gap in an internal edge case** — an overflow format, a defensive branch — is
  legitimately unit-test territory. Leave it there and record that the scenarios do
  not cover it, because that is precisely the part step 8 cannot lean on.
- Code that no scenario reaches even indirectly is either dead, or a feature nobody
  has described yet. Find out which.

Exit gate: the union of both suites meets the step 5 bar, and you know which parts of
the target the scenarios protect on their own.
