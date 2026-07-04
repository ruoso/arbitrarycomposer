#!/usr/bin/env python3
"""Claims register check (design doc 16).

The design docs make falsifiable promises; tests/claims/registry.tsv lists
the ones under enforcement as `<claim-id>\t<description>`. Every registered
claim must be referenced by at least one test via a comment containing
`enforces: <claim-id>`, and every `enforces:` tag must name a registered
claim — the register and the tests cannot drift apart silently.
"""

import re
import sys
from pathlib import Path

ENFORCES_RE = re.compile(r"enforces:\s*([A-Za-z0-9#_./-]+)")


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    registry_path = root / "tests" / "claims" / "registry.tsv"
    errors: list[str] = []

    registered: set[str] = set()
    if registry_path.exists():
        for line in registry_path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            registered.add(line.split("\t")[0])

    enforced: set[str] = set()
    for base in (root / "src", root / "tests"):
        if not base.is_dir():
            continue
        for path in list(base.rglob("*.cpp")) + list(base.rglob("*.hpp")):
            for match in ENFORCES_RE.finditer(path.read_text()):
                enforced.add(match.group(1))

    for claim in sorted(registered - enforced):
        errors.append(f"registered claim has no enforcing test: {claim}")
    for claim in sorted(enforced - registered):
        errors.append(f"'enforces: {claim}' tag names an unregistered claim")

    if errors:
        print("check_claims: FAILED", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    print(f"check_claims: OK ({len(registered)} claims enforced)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
