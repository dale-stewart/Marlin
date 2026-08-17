#!/usr/bin/env python3
"""
Tests for guard_wait_loops.py.

A guard that refuses commands is a thing that can be wrong in two directions, and both are
expensive: a false negative lets back the hang it exists to prevent, and a false positive stops
work with no way around it but deleting the guard. So both directions are pinned here.

These live in a file rather than in a shell one-liner for a reason worth keeping: the cases are
themselves self-matching commands, so a `bash -c` that contained them would be refused by the
guard under test. The first attempt at running these inline was.

    python3 buildroot/share/scripts/test_guard_wait_loops.py
"""

import json
import subprocess
import sys
from pathlib import Path

GUARD = Path(__file__).with_name("guard_wait_loops.py")

DENY = "deny"
ALLOW = "allow"

CASES = [
    # The two hangs this exists to prevent, in the exact form they were written.
    (DENY, "self-matching until+pgrep",
     'until ! pgrep -f "make unit-test-all-local" >/dev/null 2>&1; do sleep 45; done'),
    (DENY, "self-matching while+pgrep",
     'while pgrep -f "make unit-test-coverage" >/dev/null; do sleep 30; done'),

    # The case the documented `[m]ake` escape does *not* cover: the command legitimately
    # contains the target path, so the bracketed pattern still matches it.
    (DENY, "pkill -f matching a path in the same command",
     'pkill -f "[t]esthal_native_coverage" 2>/dev/null; ./.pio/build/testhal_native_coverage/program'),

    # Unbounded polls, whatever they poll for.
    (DENY, "unbounded artifact wait", 'until [ -s /tmp/out ]; do sleep 20; done'),
    (DENY, "unbounded process wait", 'until ! pgrep -f "platformio" >/dev/null; do sleep 10; done'),

    # Command position is not only the start of a line.
    (DENY, "pgrep inside a substitution",
     'for p in $(pgrep -f "platformio run"); do kill "$p"; done'),
    (DENY, "pkill after sudo", 'sudo pkill -f "platformio run"'),

    # ---- must not be refused ----

    # The documented escape, used to *list* rather than to wait.
    (ALLOW, "bracketed listing", 'pgrep -af "[t]esthal_native_coverage" | head'),

    # Prose about the tool is not a use of it. The commit introducing this guard was refused
    # by it for exactly this reason.
    (ALLOW, "prose describing the rule",
     'git commit -m "refuse a pgrep/pkill -f pattern that matches the command containing it"'),
    (ALLOW, "prose recounting the bug",
     'git commit -m "pkill -f still killed its own shell twice, once via a path"'),

    # The sanctioned ways to wait.
    (ALLOW, "wait_for.sh", 'buildroot/share/scripts/wait_for.sh .pio/mutation/results.json'),
    (ALLOW, "loop with a ceiling", "timeout 1800 bash -c 'until [ -s /tmp/x ]; do sleep 20; done'"),
    (ALLOW, "counted loop", 'for i in $(seq 1 60); do sleep 30; done'),

    # Ordinary work, which must stay unaffected.
    (ALLOW, "long build in the background", 'timeout 2400 make unit-test-all-local > /tmp/all.log 2>&1'),
    (ALLOW, "kill by pid", 'kill 1040754 1044864 2>/dev/null'),
    (ALLOW, "pgrep without -f cannot match itself", 'pgrep mutation_test.py >/dev/null && echo alive'),
    (ALLOW, "a bare sleep is not a loop", 'sleep 30; tail -1 /tmp/all.log'),
    (ALLOW, "ordinary command", 'ls -la .pio/coverage'),
]


def decide(command):
    payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": command}})
    result = subprocess.run([sys.executable, str(GUARD)], input=payload,
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"guard exited {result.returncode}: {result.stderr}")
    return DENY if result.stdout.strip() else ALLOW


def main():
    failures = []
    for expected, name, command in CASES:
        actual = decide(command)
        mark = "ok" if actual == expected else "FAIL"
        if actual != expected:
            failures.append((name, expected, actual))
        print(f"  {mark:4}  {expected:5}  {name}")

    print()
    if failures:
        for name, expected, actual in failures:
            print(f"FAILED: {name}: expected {expected}, got {actual}")
        return 1
    print(f"{len(CASES)} cases, all as expected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
