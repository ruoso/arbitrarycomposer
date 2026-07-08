#!/usr/bin/env python3
"""RT-safety grep-lint (audio.rt_safety, design doc 16:194-198, Decision D5).

The non-Clang backstop for the device RT callback chain. Doc 16's boundary rule
-- "no allocation in `[[clang::nonblocking]]` call graphs" -- "starts as a
grep-based lint script in the gate and graduates to clang-tidy/clang-query". This
is that script: it finds every function annotated with `ARBC_RT_NONBLOCKING`
(the portable spelling, arbc/base/rt_safety.hpp) and asserts its body carries no
forbidden token -- a heap allocation, a lock, or a refcounted-pointer construction.

It is deliberately conservative: it scans only the annotated function BODY (not
the signature, not the surrounding TU), after stripping comments and string
literals so prose like "lock-free" or "no allocation" never trips it. It cannot
see through a call graph (an indirect allocation in a callee slips past), which is
exactly why it is the backstop and RealtimeSanitizer (Layer A) plus the RtScope
guard (Layer B) are the primary checks -- but it gives every push a cheap signal
on the lanes that carry neither.
"""

import re
import sys
from pathlib import Path

# Forbidden CODE tokens inside an annotated body (checked post comment/string
# strip so they match code, not prose). Each is a blocking / allocating / refcount
# primitive the [[clang::nonblocking]] contract forbids.
FORBIDDEN = [
    (re.compile(r"\bnew\b"), "heap allocation (new)"),
    (re.compile(r"\bmalloc\b"), "heap allocation (malloc)"),
    (re.compile(r"\bcalloc\b"), "heap allocation (calloc)"),
    (re.compile(r"\brealloc\b"), "heap allocation (realloc)"),
    (re.compile(r"\bmake_shared\b"), "refcount allocation (make_shared)"),
    (re.compile(r"\bmake_unique\b"), "heap allocation (make_unique)"),
    (re.compile(r"\bshared_ptr\b"), "refcounted pointer (shared_ptr)"),
    (re.compile(r"\block_guard\b"), "lock (lock_guard)"),
    (re.compile(r"\bunique_lock\b"), "lock (unique_lock)"),
    (re.compile(r"\bscoped_lock\b"), "lock (scoped_lock)"),
    (re.compile(r"\bstd::mutex\b"), "lock (std::mutex)"),
    (re.compile(r"\.lock\s*\("), "lock (.lock())"),
]

MARKER = "ARBC_RT_NONBLOCKING"


def strip_comments(text: str) -> str:
    """Replace // and /* */ comments and string-literal contents with spaces,
    preserving newlines and total length so byte offsets still map to lines."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
        elif c == '"':
            out.append('"')
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append('"')
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def body_span(text: str, start: int) -> tuple[int, int] | None:
    """From `start` (just past the marker) find the annotated body: the first
    brace-matched `{...}`. Returns (open, close) offsets, or None for a
    declaration (a `;` reached before any `{`)."""
    i, n = start, len(text)
    while i < n and text[i] not in "{;":
        i += 1
    if i >= n or text[i] == ";":
        return None  # a declaration, not a definition -- no body to scan
    depth = 0
    open_at = i
    while i < n:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return (open_at, i)
        i += 1
    return None


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    errors: list[str] = []
    annotated = 0

    for base in (root / "src", root / "plugins"):
        if not base.is_dir():
            continue
        for path in sorted(list(base.rglob("*.cpp")) + list(base.rglob("*.hpp"))):
            # The macro's own definition site is not an annotated function.
            if path.name == "rt_safety.hpp":
                continue
            code = strip_comments(path.read_text())
            for m in re.finditer(re.escape(MARKER), code):
                span = body_span(code, m.end())
                if span is None:
                    continue  # a declaration; its definition is scanned separately
                annotated += 1
                open_at, close_at = span
                body = code[open_at:close_at]
                for pattern, label in FORBIDDEN:
                    for hit in pattern.finditer(body):
                        line = code.count("\n", 0, open_at + hit.start()) + 1
                        rel = path.relative_to(root)
                        errors.append(
                            f"{rel}:{line}: forbidden {label} in an "
                            f"ARBC_RT_NONBLOCKING function body"
                        )

    if errors:
        print("check_rt_safety: FAILED", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    if annotated == 0:
        print("check_rt_safety: FAILED (no ARBC_RT_NONBLOCKING functions found)", file=sys.stderr)
        return 1
    print(f"check_rt_safety: OK ({annotated} annotated function bodies clean)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
