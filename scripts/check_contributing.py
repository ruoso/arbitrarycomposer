#!/usr/bin/env python3
"""CONTRIBUTING.md anti-drift check (quality.contributing_doc, design doc 16).

CONTRIBUTING.md is the one document in the tree whose entire value is that its
commands work. Its failure mode is not bad prose -- it is a preset renamed in
CMakePresets.json, a check script moved or dropped, or a relative link rotted,
leaving the document confidently wrong about how to build, test, and gate. A doc
that instructs contributors to mechanize their work while being itself unmechanized
is self-refuting, so the three things it asserts about the tree are checked:

  1. Every `scripts/<name>` it names exists. If the document shows the script being
     *invoked directly* -- the command word of a line in a fenced code block, e.g.
     `scripts/gate` or `ARBC_GATE_PRESET=asan scripts/gate` -- it must also carry the
     executable bit. A script the doc runs through an interpreter
     (`python3 scripts/check_claims.py`, the repo's convention for the check_*.py
     family, which are mode 644) only has to exist.
  2. Every `--preset <name>` and every `ARBC_GATE_PRESET=<name>` it names is a preset
     that CMakePresets.json actually defines.
  3. Every repo-relative markdown link target resolves to a real path.

Scope is deliberately those three and nothing else (Decision D8): general markdown
lint and repo-wide link checking are owned by `quality.repo_linters`, and a second,
competing markdown linter here would create the duplicate-owner problem the document
itself is written to avoid. Registered as the `docs.contributing` CTest rather than a
CI-only step, so `scripts/gate` and CI run the same check.

`--self-test` re-runs this checker as a subprocess against four synthetic documents --
a valid control plus a bogus preset, a bogus script, and a bogus link -- and asserts
the control exits 0 and each bogus one exits non-zero. Doc 16:196-197: the lint
scripts directory is a first-class part of the codebase with its own tests.
"""

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# `./scripts/x` and `scripts/x`, but not the tail of a longer path or word.
SCRIPT_RE = re.compile(r"(?<![\w/.-])(?:\./)?(scripts/[A-Za-z0-9_][A-Za-z0-9_.-]*)")
PRESET_RE = re.compile(r"--preset[ =]+([A-Za-z0-9_-]+)")
GATE_PRESET_RE = re.compile(r"ARBC_GATE_PRESET=([A-Za-z0-9_-]+)")
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
FENCE_RE = re.compile(r"^\s*```")
# A shell command line may carry `$ ` and any number of VAR=value prefixes before
# the command word.
COMMAND_PREFIX_RE = re.compile(r"^\s*(?:\$\s+)?(?:[A-Za-z_][A-Za-z0-9_]*=\S*\s+)*")


def preset_names(presets_path: Path) -> set[str]:
    """Every preset name CMakePresets.json defines, across all three preset kinds."""
    data = json.loads(presets_path.read_text())
    names: set[str] = set()
    for kind in ("configurePresets", "buildPresets", "testPresets"):
        for preset in data.get(kind, []):
            if "name" in preset:
                names.add(preset["name"])
    return names


def check_document(doc_path: Path, root: Path) -> list[str]:
    """The three drift checks. Returns `<file>:<line>: <message>` strings."""
    errors: list[str] = []
    text = doc_path.read_text()
    known_presets = preset_names(root / "CMakePresets.json")

    scripts_seen = 0
    presets_seen = 0
    links_seen = 0
    in_fence = False

    for lineno, line in enumerate(text.splitlines(), start=1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue

        # A script is *invoked* when it is the command word of a line in a fenced
        # block; anywhere else (prose, `python3 scripts/x.py`) it is merely named.
        invoked = ""
        if in_fence:
            command = line[COMMAND_PREFIX_RE.match(line).end() :]
            match = SCRIPT_RE.match(command)
            if match:
                invoked = match.group(1)

        for rel in SCRIPT_RE.findall(line):
            scripts_seen += 1
            path = root / rel
            if not path.is_file():
                errors.append(f"{doc_path.name}:{lineno}: names a script that does not exist: {rel}")
            elif rel == invoked and not (path.stat().st_mode & 0o111):
                errors.append(
                    f"{doc_path.name}:{lineno}: invokes {rel} directly, but it is not executable"
                )

        for name in PRESET_RE.findall(line) + GATE_PRESET_RE.findall(line):
            presets_seen += 1
            if name not in known_presets:
                errors.append(
                    f"{doc_path.name}:{lineno}: names a preset CMakePresets.json does not define: "
                    f"{name}"
                )

        if in_fence:
            continue
        for target in LINK_RE.findall(line):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            links_seen += 1
            # `docs/design/16-sdlc-and-quality.md#anchor` -> the file half.
            path = root / target.split("#", 1)[0]
            if not path.exists():
                errors.append(f"{doc_path.name}:{lineno}: link target does not resolve: {target}")

    # Anti-vacuity: a document naming no scripts, no presets, and no relative links is
    # one this lint would be silently guarding nothing in.
    for what, count in (("scripts", scripts_seen), ("presets", presets_seen), ("links", links_seen)):
        if count == 0:
            errors.append(f"{doc_path.name}: names no {what} at all -- this check guards nothing")

    return errors


VALID_FIXTURE = """# Fixture

```bash
scripts/gate
cmake --preset dev
```

Run `python3 scripts/check_claims.py`. See [doc 16](docs/design/16-sdlc-and-quality.md).
"""

FIXTURES = {
    "bogus-preset": VALID_FIXTURE.replace("--preset dev", "--preset nonesuch"),
    "bogus-script": VALID_FIXTURE.replace("scripts/check_claims.py", "scripts/check_nonesuch.py"),
    "bogus-link": VALID_FIXTURE.replace(
        "docs/design/16-sdlc-and-quality.md", "docs/design/99-nonesuch.md"
    ),
}


def run_self_test(root: Path) -> int:
    """Negative fixtures, run through the real entry point so exit codes are pinned."""
    checker = Path(__file__).resolve()
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        cases = [("valid-control", VALID_FIXTURE, 0)] + [
            (name, text, 1) for name, text in sorted(FIXTURES.items())
        ]
        for name, text, want in cases:
            doc = Path(tmp) / f"{name}.md"
            doc.write_text(text)
            got = subprocess.run(
                [sys.executable, str(checker), "--document", str(doc)],
                capture_output=True,
                text=True,
                cwd=root,
            ).returncode
            if got != want:
                failures.append(f"fixture {name}: expected exit {want}, got {got}")

    if failures:
        print("check_contributing: SELF-TEST FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"check_contributing: self-test OK ({len(FIXTURES)} negative fixtures + control)")
    return 0


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    args = sys.argv[1:]

    if args == ["--self-test"]:
        return run_self_test(root)

    doc_path = root / "CONTRIBUTING.md"
    if len(args) == 2 and args[0] == "--document":
        doc_path = Path(args[1])
    elif args:
        print(f"usage: {Path(__file__).name} [--document PATH | --self-test]", file=sys.stderr)
        return 2

    if not doc_path.is_file():
        print("check_contributing: FAILED", file=sys.stderr)
        print(f"  document does not exist: {doc_path}", file=sys.stderr)
        return 1

    errors = check_document(doc_path, root)
    if errors:
        print("check_contributing: FAILED", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    print(f"check_contributing: OK ({doc_path.name} names only real scripts, presets, and paths)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
