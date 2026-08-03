#!/usr/bin/env python3
"""
mutation_test.py

Mutation testing for Marlin's native unit test suites.

Coverage proves a line ran. Mutation proves something asserted on it. This script
generates single-line mutants of one source file, rebuilds and re-runs the test suite
against each, and reports which ones the tests failed to notice.

It drives the compiler and linker directly rather than going through `platformio test`
for each mutant: PlatformIO's own startup dominated the per-mutant cost, and direct
invocation is both faster and safe to run in parallel.

Usage:
    buildroot/share/scripts/mutation_test.py Marlin/src/gcode/parser.cpp
    buildroot/share/scripts/mutation_test.py <target> --env acceptance_native_test
    buildroot/share/scripts/mutation_test.py <target> --rerun-survivors results.json

See docs/legacy-rescue-plan.md and .claude/skills/legacy-rescue/ for how this fits the
wider workflow.
"""

import argparse, concurrent.futures, glob, json, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

# Status values. BUILD_FAIL is not a mutant: the text mutator emits some invalid C++,
# and a program that never compiled was never tested. Those are excluded from the score
# rather than counted as kills, which would silently inflate it.
KILLED, SURVIVED, TIMEOUT, BUILD_FAIL = 'KILLED', 'SURVIVED', 'TIMEOUT', 'BUILD_FAIL'


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, **kw)


def apply_suite_config(suite):
    """Apply a test suite's configuration.

    Marlin's test targets run `restore_configs`, which git-checkouts the configuration
    files. Any run started afterwards would otherwise compile against the stock board
    and fail on every mutant — which, if build failures are miscounted, looks like a
    perfect score.
    """
    ini = REPO / 'test' / f'001-{suite}.ini'
    if not ini.exists():
        matches = sorted((REPO / 'test').glob(f'*-{suite}.ini'))
        if not matches:
            sys.exit(f"No configuration found for suite '{suite}' in test/")
        ini = matches[0]
    shutil.copyfile(ini, REPO / 'Marlin' / 'config.ini')
    r = run([sys.executable, 'buildroot/share/PlatformIO/scripts/configuration.py'])
    if r.returncode != 0:
        sys.exit(f"Failed to apply {ini.name}:\n{r.stdout}\n{r.stderr}")


def build_baseline(env, suite):
    """Build the suite once so object files and the compile database exist.

    Order matters. preflight-checks.py deletes a few object files on every build (M115
    and Warnings, to refresh their timestamps), so the compile database must be
    generated *before* the test build — otherwise those objects are left missing and
    the link fails with undefined references.
    """
    print(f"Building {env} ({suite})...", flush=True)
    r = run(['pio', 'run', '-t', 'compiledb', '-e', env])
    if r.returncode != 0:
        sys.exit(f"Could not generate compile_commands.json:\n{r.stdout}\n{r.stderr}")
    r = run(['platformio', 'test', '-e', env, '-f', suite])
    out = r.stdout + r.stderr
    if r.returncode != 0 or 'ERRORED' in out:
        sys.exit(f"Baseline build/test is not green — refusing to run.\n{out[-3000:]}")


def compile_command_for(target):
    db = json.loads((REPO / 'compile_commands.json').read_text())
    for e in db:
        if Path(e['file']).resolve() == (REPO / target).resolve():
            return e.get('command') or ' '.join(e['arguments'])
    sys.exit(f"{target} is not in compile_commands.json — is it compiled into this env?")


def build_dir(env):
    return REPO / '.pio' / 'build' / env


def link_inputs(env):
    """Object files and archives for the test binary.

    NOTE: link order matters here — sorting the object list produces a binary that
    segfaults on start, so the discovery order is captured once and reused for every
    mutant. The baseline gate below is what proves the captured order is good.
    """
    objs = [str(p) for p in build_dir(env).rglob('*.o')]
    libs = [str(p) for p in build_dir(env).rglob('*.a')]
    return objs, libs


# Marlin's Unity main() exits 0 even when assertions fail — PlatformIO decides pass/fail
# by parsing this summary line, and so must we. Classifying on the exit code alone marks
# every failing mutant as a survivor.
UNITY_SUMMARY = re.compile(r'(\d+) Tests (\d+) Failures (\d+) Ignored')


def suite_failed(rc, output):
    """True if the test binary reported a failure, by whatever means."""
    if rc != 0:
        return True                     # crash, abort, or non-zero exit
    m = UNITY_SUMMARY.search(output)
    if not m:
        return True                     # no summary at all: treat as failure, not success
    return int(m.group(2)) > 0


def link_and_run(objs, libs, target_obj, mutant_obj, out_binary, timeout, diag=None):
    """
    Link the suite with one object substituted, run it, and report (rc, output).

    `diag` is an optional dict filled in with what happened, for callers that must
    explain a failure rather than just classify it. A mutant only needs the verdict; the
    baseline gate needs the reason, and stdout alone is empty in the two cases that
    matter most — a link error and a binary that dies before Unity prints anything.
    """
    linked = [mutant_obj if os.path.realpath(o) == os.path.realpath(target_obj) else o for o in objs]
    r = subprocess.run(['g++', '-o', out_binary] + linked + libs + ['-lrt', '-lpthread'],
                       cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0:
        if diag is not None:
            diag.update(phase='link', returncode=r.returncode, stderr=r.stderr)
        return None, r.stderr
    try:
        p = subprocess.run([out_binary], cwd=REPO, capture_output=True, text=True, timeout=timeout)
        if diag is not None:
            diag.update(phase='run', returncode=p.returncode, stdout=p.stdout, stderr=p.stderr)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        if diag is not None:
            diag.update(phase='run', returncode='timeout', stdout='', stderr='')
        return 'timeout', ''


def explain_baseline_failure(diag):
    """
    Say why the baseline is not green, and name the likely cause.

    "Not green" covers several very different faults, and the two most common ones both
    leave stdout empty, so printing stdout alone explains nothing. Each branch below is a
    failure that has actually happened here.
    """
    phase, rc = diag.get('phase'), diag.get('returncode')
    err, outp = (diag.get('stderr') or '').strip(), (diag.get('stdout') or '').strip()

    if phase == 'link':
        hint = ("Undefined references to symbols the suite does define usually mean object\n"
                "  files were deleted by a build hook after the object list was captured —\n"
                "  rebuild the test env, then re-run.")
        return f"The link failed (g++ exit {rc}).\n  {hint}\n\n{err[-2000:]}"

    if rc == 'timeout':
        return ("The baseline binary hung. Something in the suite waits forever without a\n"
                "  test failing — a busy-wait on I/O, or a loop no simulated clock advances.\n"
                "  Find it before measuring: every mutant would be scored against a hang.")

    if isinstance(rc, int) and rc < 0:
        return (f"The baseline binary died on signal {-rc} before finishing"
                f"{' and printed nothing' if not outp else ''}.\n"
                "  A crash at startup with no output is usually static-initialisation order:\n"
                "  the link order here is load-bearing, and reordering the object list can\n"
                "  segfault a binary that is otherwise correct.\n"
                f"\n{(outp or err)[-2000:]}")

    if isinstance(rc, int) and rc != 0:
        return f"The baseline binary exited {rc}.\n\n{(outp or err)[-2000:]}"

    if not UNITY_SUMMARY.search(outp):
        return ("The baseline binary exited 0 but printed no test summary, so there is no\n"
                "  evidence any test ran. Treating that as failure rather than success.\n"
                f"\n{outp[-2000:] or '(no output at all)'}")

    return f"The baseline suite reported failures.\n\n{outp[-2000:]}"


def evaluate(mutant_path, line, ctx):
    """Compile, link and run one mutant. Returns a result record."""
    tmpdir = tempfile.mkdtemp(prefix='mut_')
    try:
        obj = os.path.join(tmpdir, 'mutant.o')
        binary = os.path.join(tmpdir, 'program')

        # Compile the mutated translation unit in place of the original. The mutant
        # lives elsewhere on disk, so the original's directory is added to the include
        # path — quoted includes resolve relative to the source file, not the cwd.
        cmd = re.sub(r'-o\s+\S+\.o', f'-o {obj}', ctx['compile_cmd'])
        cmd = cmd.replace(ctx['target'], mutant_path)
        cmd += f" -I{os.path.dirname(ctx['target'])}"
        c = subprocess.run(cmd, shell=True, cwd=REPO, capture_output=True, text=True)
        if c.returncode != 0:
            return {'mutant': os.path.basename(mutant_path), 'line': line, 'status': BUILD_FAIL}

        rc, out = link_and_run(ctx['objs'], ctx['libs'], ctx['target_obj'], obj, binary, ctx['timeout'])
        if rc is None:
            status = BUILD_FAIL
        elif rc == 'timeout':
            status = TIMEOUT      # a mutant that hangs is a mutant the suite detected
        else:
            status = KILLED if suite_failed(rc, out) else SURVIVED
        return {'mutant': os.path.basename(mutant_path), 'line': line, 'status': status}
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def covered_lines(target, coverage_build):
    """Lines gcov recorded as executed, so mutants land where the tests actually reach."""
    if not coverage_build:
        return None
    if not (REPO / coverage_build).exists():
        print(f"warning: no coverage build at {coverage_build} — mutating ALL lines, which is\n"
              f"         slower and reports survivors on lines no test can reach. Build it with\n"
              f"         'pio run -t marlin_<suite> -e <env>_coverage' for a meaningful score.")
        return None
    out = tempfile.mktemp(suffix='.json')
    r = run(['gcovr', '-r', '.', str(coverage_build), '--filter', target, '--json', out])
    if r.returncode != 0 or not os.path.exists(out):
        print(f"warning: could not read coverage from {coverage_build}; mutating all lines")
        return None
    data = json.loads(Path(out).read_text())
    os.unlink(out)
    return {l['line_number'] for f in data['files'] for l in f['lines'] if l['count'] > 0}


def check_disk_space(target, mutants_dir):
    """
    Fail before generating rather than part-way through evaluating.

    One mutant is a full copy of the target, and a large source file yields tens of
    thousands of them — several GB, kept after the run because --rerun-survivors reads
    them back. Each worker also needs a scratch dir. Running out of space mid-flight
    surfaces as an OSError from a worker thread after most of the run is already spent.
    """
    src_bytes = (REPO / target).stat().st_size
    free = shutil.disk_usage(mutants_dir.parent).free
    # ~30k mutants for a 200 KB source has been measured; 20x the source size per 1000
    # mutants is the right order. Ask for a conservative floor plus scratch headroom.
    needed = max(2 << 30, src_bytes * 30000 // 1000 * 2)
    if free < needed:
        sys.exit(
            f"not enough free space to generate mutants for {target}\n"
            f"  free: {free / (1<<30):.1f} GiB, want at least {needed / (1<<30):.1f} GiB\n"
            f"  {mutants_dir} holds one full copy of the target per mutant and is kept\n"
            f"  after the run so --rerun-survivors can read it back. Delete it to reclaim\n"
            f"  the space; the next full run regenerates it.\n"
            f"  Or set MUTATION_MUTANT_DIR to a path on a filesystem with room."
        )


def generate_mutants(target, mutants_dir, covered):
    """Generate mutants, keeping single-line changes on lines worth mutating."""
    shutil.rmtree(mutants_dir, ignore_errors=True)
    os.makedirs(mutants_dir)
    r = run(['mutate', target, '--mutantDir', str(mutants_dir), '--noCheck'])
    if r.returncode != 0:
        sys.exit(f"mutate failed (is universalmutator installed?):\n{r.stdout}\n{r.stderr}")

    original = (REPO / target).read_text().split('\n')
    kept = []
    for m in sorted(glob.glob(os.path.join(mutants_dir, '*'))):
        lines = Path(m).read_text().split('\n')
        if len(lines) != len(original):
            continue
        diff = [i + 1 for i, (a, b) in enumerate(zip(original, lines)) if a != b]
        if len(diff) != 1:
            continue
        if covered is None or diff[0] in covered:
            kept.append((m, diff[0]))
    return kept


def report(results, total_generated):
    counts = {s: sum(1 for r in results if r['status'] == s) for s in (KILLED, SURVIVED, TIMEOUT, BUILD_FAIL)}
    testable = len(results) - counts[BUILD_FAIL]
    detected = counts[KILLED] + counts[TIMEOUT]

    print()
    print(f"  generated        {total_generated}")
    print(f"  run              {len(results)}")
    print(f"  build failures   {counts[BUILD_FAIL]}  (excluded: never produced a testable program)")
    print(f"  testable         {testable}")
    print(f"  killed           {counts[KILLED]}")
    print(f"  timed out        {counts[TIMEOUT]}  (counted as detected)")
    print(f"  survived         {counts[SURVIVED]}")
    if testable:
        score = 100.0 * detected / testable
        print(f"\n  mutation score   {detected}/{testable} = {score:.1f}%")
        if score > 100:
            sys.exit("Score above 100% — the harness is broken, not the suite.")
    if counts[SURVIVED]:
        by_line = {}
        for r in results:
            if r['status'] == SURVIVED:
                by_line[r['line']] = by_line.get(r['line'], 0) + 1
        print("\n  survivors by line (investigate the largest clusters first;")
        print("  a cluster is usually one missing input class, not one missing assertion):")
        for line, n in sorted(by_line.items(), key=lambda kv: -kv[1])[:15]:
            print(f"    line {line:<6} {n}")
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('target', help='source file to mutate, e.g. Marlin/src/gcode/parser.cpp')
    ap.add_argument('--env', default='linux_native_test', help='PlatformIO environment (default: %(default)s)')
    ap.add_argument('--suite', default='default', help='test suite from test/*.ini (default: %(default)s)')
    ap.add_argument('--coverage-build', default=None,
                    help='gcov build dir used to restrict mutants to covered lines '
                         '(default: the matching *_coverage env; "" to disable)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--timeout', type=int, default=0,
                    help='seconds per mutant run (default: 20x the measured baseline, min 30)')
    ap.add_argument('--results', default='.pio/mutation/results.json')
    ap.add_argument('--rerun-survivors', metavar='RESULTS',
                    help='re-run only the survivors from a previous results file')
    ap.add_argument('--fail-on-survivors', action='store_true',
                    help='exit non-zero if any mutant survived (for CI gating); by default '
                         'a completed run exits 0 and survivors are reported, since finding '
                         'them is the normal outcome mid-rescue')
    args = ap.parse_args()

    target = args.target
    if not (REPO / target).exists():
        sys.exit(f"No such file: {target}")

    apply_suite_config(args.suite)
    build_baseline(args.env, args.suite)

    compile_cmd = compile_command_for(target)
    target_obj = re.search(r'-o\s+(\S+\.o)', compile_cmd).group(1)
    objs, libs = link_inputs(args.env)
    ctx = {'compile_cmd': compile_cmd, 'target': target, 'target_obj': str(REPO / target_obj),
           'objs': objs, 'libs': libs, 'timeout': args.timeout or 600, 'covered_count': 0}

    # Baseline gate: link and run the unmutated objects exactly the way every mutant
    # will be linked and run. Without this, anything that breaks the pipeline scores
    # every mutant as killed.
    with tempfile.TemporaryDirectory() as td:
        diag = {}
        t0 = time.monotonic()
        rc, out = link_and_run(objs, libs, ctx['target_obj'], ctx['target_obj'],
                               os.path.join(td, 'baseline'), ctx['timeout'], diag)
        baseline_s = time.monotonic() - t0
        if rc is None or rc == 'timeout' or suite_failed(rc, out):
            why = explain_baseline_failure(diag)
            sys.exit(f"Baseline binary is not green — refusing to run. Every mutant would "
                     f"look killed.\n\n{why}")

    # A timeout counts as *detected*, so one that is too tight turns a survivor into a
    # false kill and silently flatters the score — and how tight it is depends on machine
    # load, which makes the metric depend on what else was running. Scale it from the
    # suite's own measured runtime instead of guessing a constant. Measured here: mutants
    # that survive at a generous timeout were being recorded as TIMEOUT under load at 30s.
    if not args.timeout:
        ctx['timeout'] = max(30, int(baseline_s * 20) + 1)
    print(f"baseline green in {baseline_s:.1f}s — {ctx['timeout']}s per mutant"
          f"{' (from --timeout)' if args.timeout else ''}", flush=True)

    # One full copy of the target per mutant, several GB for a large source file, kept
    # after the run so --rerun-survivors can read it back. MUTATION_MUTANT_DIR moves that
    # off the repo's filesystem when it is short of space — a run per worktree multiplies
    # it. The results JSON stores bare filenames, so a re-run must point at the same dir.
    mutants_dir = Path(os.environ.get('MUTATION_MUTANT_DIR') or (REPO / '.pio' / 'mutation' / 'mutants'))
    os.makedirs(mutants_dir.parent, exist_ok=True)
    if args.rerun_survivors:
        previous = json.loads(Path(args.rerun_survivors).read_text())
        if isinstance(previous, dict):
            ctx['covered_count'] = previous.get('covered_lines', 0)
            previous = previous['results']
        else:
            ctx['covered_count'] = 0
        mutants = [(str(mutants_dir / r['mutant']), r['line']) for r in previous if r['status'] == SURVIVED]
        total_generated = len(mutants)
        missing = [m for m, _ in mutants if not os.path.exists(m)]
        if missing:
            sys.exit(f"{len(missing)} of {len(mutants)} survivor mutants are missing from "
                     f"{mutants_dir}\n  (e.g. {missing[0]})\n"
                     f"  The directory was cleaned, or generated for a different target.\n"
                     f"  Re-run without RERUN= to regenerate before re-running survivors.")
        print(f"re-running {len(mutants)} survivors from {args.rerun_survivors}")
    else:
        check_disk_space(target, mutants_dir)
        cov_build = args.coverage_build
        if cov_build is None:
            cov_build = f".pio/build/{args.env.replace('_test', '_coverage')}"
        covered = covered_lines(target, cov_build or None)
        ctx['covered_count'] = len(covered) if covered else 0
        mutants = generate_mutants(target, mutants_dir, covered)
        total_generated = len(glob.glob(str(mutants_dir / '*')))
        scope = f"{len(covered)} covered lines" if covered else "all lines"
        print(f"{total_generated} mutants generated, {len(mutants)} on {scope}")

    if not mutants:
        sys.exit("No mutants to run.")

    results = []
    print(f"running {len(mutants)} mutants across {args.jobs} workers...", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(evaluate, m, line, ctx): m for m, line in mutants}
        for n, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            results.append(fut.result())
            if n % 50 == 0:
                print(f"  {n}/{len(mutants)}", flush=True)

    results.sort(key=lambda r: (r['line'], r['mutant']))
    out = REPO / args.results
    out.parent.mkdir(parents=True, exist_ok=True)
    # Record what population this result came from. Two runs are only comparable if they
    # mutated the same lines: when coverage grows, new mutants appear and the score moves
    # for reasons that have nothing to do with the tests. Without this it is easy to
    # compare a result against one taken from a different covered-line set and read the
    # difference as a regression.
    out.write_text(json.dumps({
        'target': target, 'env': args.env, 'suite': args.suite,
        'covered_lines': ctx['covered_count'], 'generated': total_generated,
        # A timeout counts as detected, so two runs at different thresholds are not
        # comparable either — record it next to the covered-line set for the same reason.
        'timeout_s': ctx['timeout'], 'baseline_s': round(baseline_s, 2),
        'run': len(mutants), 'results': results,
    }, indent=1))
    counts = report(results, total_generated)
    print(f"\n  covered lines    {ctx['covered_count']}  (results are only comparable "
          f"between runs with the same covered-line set)")
    print(f"  mutant timeout   {ctx['timeout']}s, from a {baseline_s:.1f}s baseline "
          f"(a timeout scores as detected, so this must match too)")
    print(f"  results: {args.results}")
    return 1 if (args.fail_on_survivors and counts[SURVIVED]) else 0


if __name__ == '__main__':
    sys.exit(main())
