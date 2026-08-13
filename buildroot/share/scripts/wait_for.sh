#!/usr/bin/env bash
#
# Wait for a file to appear, then exit.
#
# Use this instead of polling the process table. A loop written as
#
#     while pgrep -f "make unit-test-all-local" >/dev/null; do sleep 30; done
#
# never finishes: `pgrep -f` searches whole command lines, and the waiter's own command
# line contains the string it is searching for, so it finds itself. Start a second one and
# they find each other. On 2026-08-13 ten of them piled up in one session, all reporting
# "still running" about builds that had finished, and the reports were believed.
#
# A build's output cannot match the thing looking for it, and its existence is the
# condition you actually care about.
#
#   wait_for.sh .pio/mutation/results.json           # default 2h limit, 15s poll
#   wait_for.sh -t 600 -i 5 .pio/coverage/summary.txt
#
# Exits 0 when the file appears, 1 on timeout — so a hung job fails rather than passing
# quietly, which is the whole point of having a limit.
#
# Note the file must not already exist when the job starts, or this returns immediately
# and says the job is done when it has not begun. Delete it first, or wait on a path the
# job creates fresh.

set -u

timeout=7200
interval=15

while getopts "t:i:" opt; do
  case "$opt" in
    t) timeout=$OPTARG ;;
    i) interval=$OPTARG ;;
    *) echo "usage: $0 [-t seconds] [-i seconds] <path>" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))

if [ $# -ne 1 ]; then
  echo "usage: $0 [-t seconds] [-i seconds] <path>" >&2
  exit 2
fi

target=$1
waited=0

while [ ! -e "$target" ]; do
  if [ "$waited" -ge "$timeout" ]; then
    echo "wait_for: gave up after ${timeout}s; $target never appeared" >&2
    exit 1
  fi
  sleep "$interval"
  waited=$((waited + interval))
done

echo "wait_for: $target appeared after ${waited}s"
