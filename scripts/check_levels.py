#!/usr/bin/env python3
"""Levelization check (design docs 16/17).

Validates two things against the doc 17 component table:
  1. Declared DEPENDS edges in each src/<component>/CMakeLists.txt are a
     subset of that component's allowed direct dependencies.
  2. Cross-component includes (#include <arbc/<other>/...>) resolve only
     to components within the transitive closure of declared DEPENDS.
"""

import re
import sys
from pathlib import Path

# The doc 17 table: component -> allowed *direct* dependencies.
# (Directory/target names use underscores; prose uses dashes.)
ALLOWED = {
    "base": set(),
    "pool": {"base"},
    "media": {"base"},
    "surface": {"base", "media"},
    "model": {"base", "pool", "media"},
    "contract": {"base", "pool", "media", "surface", "model"},
    "cache": {"base", "surface"},
    "backend_cpu": {"base", "media", "surface"},
    "compositor": {"contract", "cache"},
    "audio_engine": {"contract", "cache"},
    "serialize": {"contract", "model"},
    "kind_solid": {"contract"},
    "kind_tone": {"contract"},
    "kind_raster": {"contract"},
    "kind_fade": {"contract"},
    "kind_crossfade": {"contract"},
    "kind_nested": {"contract"},
    "runtime": {
        "base", "pool", "media", "surface", "model", "contract", "cache",
        "backend_cpu", "compositor", "audio_engine", "serialize",
        "kind_solid", "kind_tone", "kind_raster", "kind_fade",
        "kind_crossfade", "kind_nested",
    },
}

COMPONENT_RE = re.compile(
    r"arbc_add_component\((.*?)\)", re.DOTALL | re.IGNORECASE
)
NAME_RE = re.compile(r"\bNAME\s+(\S+)")
DEPENDS_RE = re.compile(r"\bDEPENDS\s+((?:(?!SOURCES|PUBLIC_HEADERS)\S+\s*)+)")
INCLUDE_RE = re.compile(r'#include\s+[<"]arbc/([A-Za-z0-9_]+)/')


def closure(component: str, declared: dict[str, set[str]]) -> set[str]:
    seen: set[str] = set()
    stack = list(declared.get(component, set()))
    while stack:
        dep = stack.pop()
        if dep in seen:
            continue
        seen.add(dep)
        stack.extend(declared.get(dep, set()))
    return seen


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    src = root / "src"
    errors: list[str] = []
    declared: dict[str, set[str]] = {}

    if not src.is_dir():
        print("check_levels: no src/ yet; nothing to check")
        return 0

    for lists in sorted(src.glob("*/CMakeLists.txt")):
        text = lists.read_text()
        for match in COMPONENT_RE.finditer(text):
            body = match.group(1)
            name_m = NAME_RE.search(body)
            if not name_m:
                errors.append(f"{lists}: arbc_add_component without NAME")
                continue
            name = name_m.group(1)
            deps_m = DEPENDS_RE.search(body)
            deps = set(deps_m.group(1).split()) if deps_m else set()
            declared[name] = deps
            if name not in ALLOWED:
                errors.append(f"{name}: not in the doc 17 component table")
                continue
            for dep in sorted(deps - ALLOWED[name]):
                errors.append(
                    f"{name}: DEPENDS on '{dep}' not allowed by doc 17"
                )

    for name in declared:
        allowed_includes = closure(name, declared) | {name}
        comp_dir = src / name
        for path in list(comp_dir.rglob("*.hpp")) + list(comp_dir.rglob("*.cpp")):
            for line_no, line in enumerate(path.read_text().splitlines(), 1):
                inc = INCLUDE_RE.search(line)
                if inc and inc.group(1) not in allowed_includes:
                    errors.append(
                        f"{path.relative_to(root)}:{line_no}: includes "
                        f"arbc/{inc.group(1)}/ outside dependency closure"
                    )

    if errors:
        print("check_levels: FAILED", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    print(f"check_levels: OK ({len(declared)} components)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
