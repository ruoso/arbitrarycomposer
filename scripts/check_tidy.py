#!/usr/bin/env python3
"""clang-tidy over the compile-commands database, with a per-check ratchet
(design doc 16 § C++ linting).

WHY A RATCHET AND NOT A GATE. Doc 16 wants the curated tidy profile enforced on
every push. It never got there: the nightly lane carried `continue-on-error` and
a comment promising promotion "once the baseline is clean", and the baseline was
never clean -- it opened at 49 findings on the first nightly (2026-07-05) and had
grown to 513 by 2026-07-24 without a single run ever going green, because a job
that is allowed to fail is a job nobody reads. A gate on a non-zero baseline is
unlandable; a ratchet is landable TODAY and has the property that actually
matters: the number cannot grow. Findings are fixed by class, the baseline is
lowered as they are, and the lane becomes a real gate the day it reaches zero.

The rule, deliberately asymmetric:

  - a check ABOVE its baseline fails      -- new debt, the thing this exists to stop
  - a check BELOW its baseline passes     -- with a loud note to re-record it
  - a check absent from the baseline fails if it fires at all

Asymmetric because the two directions have different failure modes. An increase
is a regression and someone must look. A decrease is either a fix (record it) or
toolchain drift in the harmless direction -- and failing the nightly for someone
ELSE'S good deed, or for a libstdc++ patch bump, teaches the team to ignore the
lane, which is precisely how the baseline reached 513 in the first place.

SCOPE COMES FROM THE COMPILE DATABASE, not from a `git ls-files` glob. The glob
this replaces was wrong in both directions: it missed the entire top-level
`tests/` tree (132 TUs) and `plugins/`, while including two `bench/` TUs that are
not configured in the `dev` preset at all -- clang-tidy analyzed those with
guessed flags, failed to find `benchmark/benchmark.h`, and recovery-parsed its
way to 27 bogus findings in a file it could not compile. A TU that is in the
database is a TU that compiles; anything else is noise dressed as debt.

Headers are analyzed too (`.clang-tidy` sets `HeaderFilterRegex`), so findings in
this codebase's substantial header-inline code are visible. That means one header
finding is reported once per TU that includes it, so diagnostics are DEDUPED by
(file, line, column, check) before counting -- otherwise the count would track
the include graph rather than the debt.

Anti-vacuity: an empty file list, a missing clang-tidy, or a TU that fails to
COMPILE (`clang-diagnostic-error`) is a hard failure, never a silent pass. A lint
that quietly checks nothing is worse than no lint, because it reads as green.

`--self-test` runs synthetic clang-tidy output through the real entry point via
`--from-log` -- a clean baseline, a regression, an improvement, a check the
baseline has never seen, and a TU that failed to compile -- and pins each exit
code. Doc 16:196-197: the lint scripts directory is a first-class part of the
codebase with its own tests.

Usage:
  scripts/check_tidy.py [--build build/dev] [-j N] [--log tidy.log]
  scripts/check_tidy.py --write-baseline    # re-record after fixing findings
  scripts/check_tidy.py --from-log tidy.log # re-score a saved run, no analysis

Needs a configured build directory (`cmake --preset dev`) for the compile
database, and clang-tidy on PATH -- pinned to 20.1.0 in the nightly lane, the
major that matches the clang the database is written by. A different major will
report a different set of checks and the ratchet will read that as debt.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "scripts" / "tidy_baseline.json"

# Test TUs: every `t/` directory, the top-level `tests/` tree, and `testing/` (the
# shipped conformance harness, which is Catch2 test bodies all the way down).
TEST_TU = re.compile(r"(^|/)t/|^(tests|testing)/")

# Two checks are unusable inside a Catch2 test body and are disabled there. The
# rule lives HERE, in one place, rather than in twenty-one copies of a
# `.clang-tidy` -- a new component's `t/` directory is covered the day it exists,
# instead of the day someone remembers to copy a config into it.
#
#   bugprone-unchecked-optional-access -- every finding was a `std::optional` used
#     after `REQUIRE(x.has_value())`. The macro is opaque to the check's dataflow,
#     so the safe idiom reads as unchecked, and the only way to satisfy it is a
#     second guard duplicating the one the REQUIRE already made. It stays ON for
#     library code, where an unchecked access is a real defect.
#   bugprone-implicit-widening-of-multiplication-result -- test literals
#     (`8 * 8 * 4`, `256U * 256U * 4U`). The overflow it guards cannot occur at
#     these magnitudes. Also ON for library code, where index math is not a
#     literal and the guard is worth having.
TEST_DISABLED_CHECKS = (
    "-bugprone-unchecked-optional-access",
    "-bugprone-implicit-widening-of-multiplication-result",
)

# `path:line:col: error|warning: message [check-name,-warnings-as-errors]`. The
# trailing `,-warnings-as-errors` is an artifact of the profile's `WarningsAsErrors`
# and is not part of the check's name.
DIAGNOSTIC = re.compile(
    r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
    r"(?:error|warning): .*? \[(?P<checks>[A-Za-z0-9_.,\-]+)\]$"
)


def compile_db_files(build: Path) -> list[Path]:
    """Every in-repo TU the build actually compiles, deduped and sorted.

    Excludes anything under the build tree itself -- FetchContent puts Catch2 and
    nlohmann_json sources there (222 of the database's 554 entries), and third-party
    code is not this repo's debt.
    """
    db_path = build / "compile_commands.json"
    if not db_path.exists():
        sys.exit(
            f"check_tidy: FAILED (no compile database at {db_path} -- "
            f"configure first: `cmake --preset dev`)"
        )
    entries = json.loads(db_path.read_text())
    files = set()
    for entry in entries:
        path = Path(entry["file"]).resolve()
        if not path.is_relative_to(ROOT) or path.is_relative_to(build.resolve()):
            continue
        if path.exists():
            files.add(path)
    return sorted(files)


def is_test_tu(path: Path) -> bool:
    """Is this TU a test body? (Repo-relative, so `/t/` means a component's tests.)"""
    return TEST_TU.search(str(path.relative_to(ROOT))) is not None


def run_one(clang_tidy: str, build: Path, path: Path) -> str:
    """Analyze one TU. Returns clang-tidy's raw output (diagnostics on stdout)."""
    command = [clang_tidy, "-p", str(build), "--quiet"]
    # `--checks` is APPENDED to the profile's list, so a leading-minus list
    # subtracts from it and leaves every other check in force.
    if is_test_tu(path):
        command.append("--checks=" + ",".join(TEST_DISABLED_CHECKS))
    command.append(str(path))
    proc = subprocess.run(
        command,
        capture_output=True,
        text=True,
        cwd=ROOT,
    )
    # clang-tidy exits 1 when a diagnostic fires (the profile is warnings-as-errors),
    # so a non-zero code is ordinary. Anything worse -- a crash, a signal -- is not:
    # that TU contributed no findings and would silently deflate the count.
    if proc.returncode not in (0, 1):
        sys.exit(
            f"check_tidy: FAILED (clang-tidy exited {proc.returncode} on {path} -- "
            f"the lane cannot count what it could not analyze)\n{proc.stderr}"
        )
    return proc.stdout + proc.stderr


def parse(output: str) -> set[tuple[str, str, str, str]]:
    """Deduped diagnostics as (repo-relative file, line, column, check)."""
    found = set()
    for line in output.splitlines():
        match = DIAGNOSTIC.match(line.strip())
        if not match:
            continue
        check = match.group("checks").split(",")[0]
        path = Path(match.group("file"))
        try:
            name = str(path.resolve().relative_to(ROOT))
        except ValueError:
            name = str(path)  # outside the repo (a system header); keep it visible
        found.add((name, match.group("line"), match.group("col"), check))
    return found


# Two findings of one check plus one of another -- enough to move a count in either
# direction, and to introduce a check the baseline has never seen.
def _diag(path: str, line: int, check: str) -> str:
    return f"{path}:{line}:7: warning: synthetic finding [{check},-warnings-as-errors]"


_AT_BASELINE = "\n".join(
    [
        _diag("src/a.cpp", 10, "performance-move-const-arg"),
        _diag("src/b.cpp", 20, "performance-move-const-arg"),
        _diag("src/c.cpp", 30, "bugprone-branch-clone"),
        # The same header finding seen from three TUs: one finding, not three.
        _diag("src/x.hpp", 40, "bugprone-branch-clone"),
        _diag("src/x.hpp", 40, "bugprone-branch-clone"),
        _diag("src/x.hpp", 40, "bugprone-branch-clone"),
    ]
)
_BASELINE_FIXTURE = {"total": 4, "checks": {"performance-move-const-arg": 2, "bugprone-branch-clone": 2}}

SELF_TEST_CASES = [
    ("at-baseline", _AT_BASELINE, 0),
    # One more of a known check: the regression this whole lint exists to catch.
    ("regressed", _AT_BASELINE + "\n" + _diag("src/d.cpp", 50, "performance-move-const-arg"), 1),
    # A check the baseline has never seen fires: also a regression (0 -> 1).
    ("new-check", _AT_BASELINE + "\n" + _diag("src/d.cpp", 50, "concurrency-mt-unsafe"), 1),
    # Fewer findings passes -- with a note. Failing here would punish the fix.
    ("improved", "\n".join(_AT_BASELINE.splitlines()[1:]), 0),
    # A TU that did not compile is a broken lane, never quietly-fewer-findings.
    ("did-not-compile", _AT_BASELINE + "\n" + _diag("src/d.cpp", 1, "clang-diagnostic-error"), 1),
]


# Which paths count as test bodies -- the classification that decides where the two
# Catch2-hostile checks are enforced. Wrong in either direction is silent: too narrow
# and a PR drowns in test noise, too wide and the checks stop guarding library code.
TEST_TU_CASES = [
    ("src/model/t/journal.t.cpp", True),
    ("src/pool/t/refs.t.cpp", True),
    ("tests/journal_history_reads_concurrency.t.cpp", True),
    ("testing/contract_tests.cpp", True),
    ("plugins/image/t/image_content.t.cpp", True),
    ("src/model/journal.cpp", False),
    ("src/runtime/document.cpp", False),
    ("plugins/image/image_content.cpp", False),
    # `testing/` only counts at the root -- a library path that merely contains the
    # word must not be swept into the test bucket.
    ("src/contract/testing_support.cpp", False),
]


def run_self_test() -> int:
    """Fixtures through the real entry point, so the exit codes are pinned."""
    checker = Path(__file__).resolve()
    failures = []
    for name, want in TEST_TU_CASES:
        got = is_test_tu(ROOT / name)
        if got != want:
            failures.append(f"is_test_tu({name}): {got}, want {want}")
    with tempfile.TemporaryDirectory() as tmp:
        baseline = Path(tmp) / "baseline.json"
        baseline.write_text(json.dumps(_BASELINE_FIXTURE))
        for name, log_text, want in SELF_TEST_CASES:
            log = Path(tmp) / f"{name}.log"
            log.write_text(log_text + "\n")
            got = subprocess.run(
                [sys.executable, str(checker), "--from-log", str(log), "--baseline", str(baseline)],
                capture_output=True,
                text=True,
            ).returncode
            if got != want:
                failures.append(f"{name}: exit {got}, want {want}")
    if failures:
        print("check_tidy: SELF-TEST FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"check_tidy: self-test OK ({len(SELF_TEST_CASES)} scoring cases, "
          f"{len(TEST_TU_CASES)} classification cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build/dev", help="configured build dir")
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4, help="parallel TUs")
    parser.add_argument("--log", help="write raw clang-tidy output here (for triage)")
    parser.add_argument("--from-log", help="score a saved run instead of analyzing")
    parser.add_argument("--baseline", default=str(BASELINE), help="baseline file")
    parser.add_argument(
        "--write-baseline",
        action="store_true",
        help="record the observed counts as the new baseline",
    )
    parser.add_argument("--self-test", action="store_true", help="run the fixtures")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    baseline_path = Path(args.baseline)
    if args.from_log:
        raw = Path(args.from_log).read_text()
        analyzed = f"the saved run in {args.from_log}"
    else:
        clang_tidy = shutil.which("clang-tidy")
        if clang_tidy is None:
            sys.exit("check_tidy: FAILED (clang-tidy not on PATH)")

        build = Path(args.build)
        build = build.resolve() if build.is_absolute() else (ROOT / build).resolve()
        files = compile_db_files(build)
        if not files:
            sys.exit(
                f"check_tidy: FAILED (no in-repo TUs in {build}/compile_commands.json -- "
                f"this lint would pass by checking nothing)"
            )
        tests = sum(1 for f in files if is_test_tu(f))
        library = len(files) - tests
        # Anti-vacuity for the split itself: if a pattern change swept every TU into
        # the test bucket, the two checks above would be off tree-wide and the count
        # would just quietly drop. Both buckets must be non-empty.
        if tests == 0 or library == 0:
            sys.exit(
                f"check_tidy: FAILED (TEST_TU matched {tests} of {len(files)} TUs -- "
                f"the test/library split is broken, so the checks it scopes are not "
                f"being enforced where they should be)"
            )
        print(f"check_tidy: analyzing {len(files)} TUs from {build}/compile_commands.json "
              f"({library} library, {tests} test; {args.j} at a time)", flush=True)

        with ThreadPoolExecutor(max_workers=args.j) as pool:
            outputs = list(pool.map(lambda f: run_one(clang_tidy, build, f), files))
        raw = "".join(outputs)
        analyzed = f"{len(files)} TUs"
        if args.log:
            Path(args.log).write_text(raw)

    diagnostics = parse(raw)
    observed = Counter(check for _, _, _, check in diagnostics)

    # A TU that does not compile is a broken lane, not debt: its findings are
    # whatever the recovery parser hallucinated, and its real ones are missing.
    broken = sorted({d[0] for d in diagnostics if d[3].startswith("clang-diagnostic")})
    if broken:
        print("check_tidy: FAILED (these TUs did not compile under clang-tidy; the "
              "database and the analyzer disagree)", file=sys.stderr)
        for name in broken:
            print(f"  {name}", file=sys.stderr)
        return 1

    if args.write_baseline:
        baseline_path.write_text(
            json.dumps({"total": sum(observed.values()), "checks": dict(sorted(observed.items()))}, indent=2)
            + "\n"
        )
        print(f"check_tidy: wrote {baseline_path} "
              f"({sum(observed.values())} findings across {len(observed)} checks)")
        return 0

    if not baseline_path.exists():
        sys.exit(f"check_tidy: FAILED (no baseline at {baseline_path} -- seed it with --write-baseline)")
    baseline = Counter(json.loads(baseline_path.read_text())["checks"])

    regressions = []
    improvements = []
    for check in sorted(set(observed) | set(baseline)):
        now, was = observed[check], baseline[check]
        if now > was:
            regressions.append((check, was, now))
        elif now < was:
            improvements.append((check, was, now))

    print(f"check_tidy: {sum(observed.values())} findings "
          f"(baseline {sum(baseline.values())}) over {analyzed}")
    for check, was, now in improvements:
        print(f"  improved: {check}: {was} -> {now}")
    if improvements:
        print("check_tidy: NOTE the baseline is now loose -- re-record it with "
              "`scripts/check_tidy.py --write-baseline` so the ratchet keeps holding "
              "at the new number.")

    if regressions:
        print("check_tidy: FAILED (new findings above the recorded baseline)", file=sys.stderr)
        for check, was, now in regressions:
            print(f"  {check}: {was} -> {now}", file=sys.stderr)
            # The baseline records counts, not locations, so the new finding cannot be
            # named -- it is among these. Capped, because a check with 290 findings
            # would otherwise bury the verdict under its own history.
            sites = [f"{n}:{ln}:{c}" for n, ln, c, chk in sorted(diagnostics) if chk == check]
            for site in sites[:20]:
                print(f"      {site}", file=sys.stderr)
            if len(sites) > 20:
                print(f"      ... and {len(sites) - 20} more (full output in tidy.log)", file=sys.stderr)
        print("  Fix them, or -- if the finding is a deliberate exception -- add a "
              "`NOLINT(check) // reason` and say why (doc 16:188).", file=sys.stderr)
        return 1

    print("check_tidy: OK (no check above its baseline)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
