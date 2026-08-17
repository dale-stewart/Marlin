#!/usr/bin/env python3
"""
Refuse shell commands that wait in ways known to hang here.

This is a `PreToolUse` hook on Bash. It exists because the two failure modes below are
documented in CLAUDE.md, documented again in wait_for.sh's own header, and were walked into
anyway — twice each in one session. A rule that has to be remembered is not a rule.

Reads the hook payload on stdin, writes a decision on stdout.

1.  **A `pgrep -f` / `pkill -f` pattern that matches the command containing it.**

    `pgrep -f` matches whole command lines, and the searching process has a command line of
    its own. So `while pgrep -f "make foo"; do sleep 30; done` finds itself and never
    returns, and `pkill -f "some/path"` kills its own shell.

    Detected by doing what pgrep does: compile the pattern and search the command text with
    it. That is exact rather than approximate, and it makes the documented escape work for
    free — `[m]ake foo` is a regex matching `make foo`, which does not appear in a command
    that only contains the bracketed spelling.

2.  **A polling loop with no upper bound.**

    Every wait here is on a job that can die, and an unbounded waiter outlives it. Two were
    left spinning for eleven hours; backgrounded, they reported nothing, so nothing looked
    wrong until someone counted the shells.

    A bound is `timeout N`, `wait_for.sh` (which has its own limit), or a counted loop.

Exit status is always 0: the decision is carried in the JSON, so a crash in here fails open
rather than blocking every command in the session.
"""

import json
import re
import sys

# `while`/`until` at the start of a command position, some `sleep`, and a `done` to close it.
# All three are required so that prose in a commit message cannot look like a loop.
LOOP = re.compile(r"(?:^|[;&|(\n])\s*(?:while|until)\s")
SLEEP = re.compile(r"(?:^|[;&|(\n\s])sleep\s")
DONE = re.compile(r"(?:^|[;&|(\n\s])done\b")

# `pgrep -af`, `pkill -9 -f`, `pgrep --full` … then a quoted or bare pattern.
#
# Only where the name is in *command position*, so that prose describing the tool is not read as
# a use of it. This is not hypothetical: the commit that introduced this guard was refused by it,
# because the message explains what "a pgrep/pkill -f pattern" is.
#
# Command position is the start, a separator, a substitution, or one of the words that can
# precede a command — `while pgrep`, `until ! pgrep`, `xargs pkill`, `sudo pkill`.
LEADIN = r"(?:^|[;&|(\n`]|\$\()\s*(?:(?:!|while|until|if|elif|then|else|do|sudo|xargs|time|command|nohup)\s+)*"
PROCESS_MATCH = re.compile(
    LEADIN + r"(pgrep|pkill)\b((?:\s+-{1,2}[A-Za-z-]+)*)\s+"
    r"""(?:"([^"]*)"|'([^']*)'|(\S+))"""
)

# Things that put a ceiling on a wait.
BOUNDED = re.compile(r"(?:^|[;&|(\n\s])(timeout\s|wait_for\.sh\b)")
COUNTED = re.compile(r"\bfor\s+\w+\s+in\s+(?:\$\(seq\b|\{[0-9]+\.\.)")


def deny(reason):
    json.dump({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason,
        }
    }, sys.stdout)
    sys.exit(0)


def self_matching_pattern(command):
    """The pattern a pgrep/pkill would search with, if it would find this command itself."""
    for match in PROCESS_MATCH.finditer(command):
        flags = match.group(2) or ""
        if "f" not in flags.replace("-", "") and "full" not in flags:
            continue  # not matching against full command lines, so it cannot find itself
        pattern = match.group(3) or match.group(4) or match.group(5) or ""
        if not pattern:
            continue
        try:
            if re.search(pattern, command):
                return match.group(1), pattern
        except re.error:
            continue  # not a pattern we can reason about; let it through
    return None


def main():
    try:
        payload = json.load(sys.stdin)
    except Exception:
        sys.exit(0)

    command = (payload.get("tool_input") or {}).get("command") or ""
    if not command:
        sys.exit(0)

    found = self_matching_pattern(command)
    if found:
        tool, pattern = found
        deny(
            f"`{tool} -f` with a pattern that matches this command itself: {pattern!r}.\n"
            f"`-f` matches whole command lines, including this one, so the search finds its "
            f"own shell — a wait never returns, and a kill kills itself.\n"
            f"Options, in order of preference:\n"
            f"  - wait on the artifact instead: buildroot/share/scripts/wait_for.sh <path>\n"
            f"  - or run the job with run_in_background and let the runner report it\n"
            f"  - or act on a PID you looked up in a separate, earlier command\n"
            f"  - or, if you only want to *list* processes, bracket a character: '[{pattern[:1]}]{pattern[1:]}'"
        )

    is_loop = LOOP.search(command) and SLEEP.search(command) and DONE.search(command)
    if is_loop and not BOUNDED.search(command) and not COUNTED.search(command):
        deny(
            "A polling loop with no upper bound.\n"
            "Whatever this waits for can die, and then this waits forever. Two of these were "
            "left running for eleven hours; backgrounded, they report nothing, so nothing "
            "looks wrong.\n"
            "Give it a ceiling:\n"
            "  - buildroot/share/scripts/wait_for.sh <path>   (waits on the artifact, has its own limit)\n"
            "  - or wrap it: timeout 1800 bash -c '<loop>'\n"
            "  - or count the iterations: for i in $(seq 1 60); do ...; sleep 30; done"
        )

    sys.exit(0)


if __name__ == "__main__":
    main()
