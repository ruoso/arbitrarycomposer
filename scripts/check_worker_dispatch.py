#!/usr/bin/env python3
"""Worker-dispatch grep-lint (runtime.worker_dispatch_leaf_only, design doc 02
Threading model: "Worker dispatch is leaf-only").

Worker dispatch in this engine is a POLICY, not just a mechanism: a content may
be handed to a render worker only if it is a LEAF (empty `inputs()`). An operator
-- a fade, a crossfade, a nested composition (doc 13) -- must render inline on the
driver thread, because its `render` re-enters the `PullService`, whose tile-cache
probe/insert and descent-depth accounting are render-thread-confined. Submitting
one to a worker compiles, does not assert, and usually produces the right pixels
on a developer's machine; what it actually produces is a data race on the cache
(TSan-confirmed latent for all three operator kinds, `kinds.nested_runtime_binding`).

That rule lives in exactly one place -- `worker_backed_dispatch` in
src/runtime/worker_dispatch.cpp -- and every driver that wants parallel miss-fill
obtains its `RenderDispatch` from there. This lint keeps it that way: it makes a
SECOND submission site a build failure rather than a review convention, so a
future driver cannot re-derive (or forget) the rule the way the pre-hoist offline
lambda invited.

Two checks, because there are two ways to reach `WorkerPool::submit`:

  1. Naming `RenderTask` in code (`pool.submit(RenderTask{...})`). Only the
     helper's TU and the pool's own declaration/implementation may.
  2. Brace-initializing the task at the call (`pool.submit({content, ...})`),
     which would name no type and slip past check 1. Any `.submit({` / `->submit({`
     is rejected outright, everywhere: name the task type at every submission site
     so this lint can see it. (`lookahead_pump.cpp`'s legitimate audio submission
     already spells `AudioTask` -- audio is a different discipline entirely, doc
     12, and is deliberately NOT governed by the leaf-only rule.)

Comments and string literals are stripped first, so prose that merely mentions
`RenderTask` (`pull_service.hpp`, `audio_worker_pool.hpp`) does not trip it. Tests
are exempt: `src/runtime/t/worker_pool.t.cpp` exercises `submit` directly, which is
legitimate -- the pool is a general executor with its own claims, and the leaf-only
rule is a policy layered above it (Decision 6).
"""

import re
import sys
from pathlib import Path

# The submission type. Reaching `WorkerPool::submit` means constructing one.
RENDER_TASK = re.compile(r"\bRenderTask\b")

# A submission whose argument is a braced initializer names no type, so check 1
# cannot see what is being submitted. Forbidden at every call site.
BRACED_SUBMIT = re.compile(r"(?:\.|->)\s*submit\s*\(\s*\{")

# The single home of the rule -- the one non-test TU allowed to submit a RenderTask.
DISPATCH_HELPER = "src/runtime/worker_dispatch.cpp"

# The pool's own declaration + implementation: they DEFINE `RenderTask` and
# `submit`, they do not dispatch through them.
POOL_INTERNALS = {
    "src/runtime/arbc/runtime/worker_pool.hpp",
    "src/runtime/worker_pool.cpp",
}

ALLOWED = POOL_INTERNALS | {DISPATCH_HELPER}


def strip_comments(text: str) -> str:
    """Replace // and /* */ comments and string-literal contents with spaces,
    preserving newlines and total length so byte offsets still map to lines.
    Shared idiom with scripts/check_rt_safety.py."""
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


def is_test(rel: Path) -> bool:
    """Component unit tests live in src/<component>/t/; the pool's own contract is
    legitimately tested through `submit` directly."""
    return "t" in rel.parts or rel.name.endswith(".t.cpp")


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    errors: list[str] = []
    submissions_in_helper = 0

    for base in (root / "src", root / "plugins"):
        if not base.is_dir():
            continue
        for path in sorted(list(base.rglob("*.cpp")) + list(base.rglob("*.hpp"))):
            rel = path.relative_to(root)
            if is_test(rel):
                continue
            code = strip_comments(path.read_text())
            posix = rel.as_posix()

            for m in RENDER_TASK.finditer(code):
                line = code.count("\n", 0, m.start()) + 1
                if posix == DISPATCH_HELPER:
                    submissions_in_helper += 1
                elif posix not in ALLOWED:
                    errors.append(
                        f"{rel}:{line}: `RenderTask` named outside the worker-dispatch "
                        f"helper. Worker dispatch is LEAF-ONLY (doc 02 § Threading model): "
                        f"submitting an operator content to a worker races the tile cache. "
                        f"Do not hand-roll a submission -- obtain the dispatch from "
                        f"`worker_backed_dispatch(pool)` (arbc/runtime/worker_dispatch.hpp), "
                        f"which enforces the rule."
                    )

            for m in BRACED_SUBMIT.finditer(code):
                line = code.count("\n", 0, m.start()) + 1
                errors.append(
                    f"{rel}:{line}: `submit({{...}})` with a braced initializer hides the "
                    f"task type from this lint. Name the task type at the call site."
                )

    if errors:
        print("check_worker_dispatch: FAILED", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    # Anti-vacuity: if the helper stopped naming `RenderTask`, the lint above is
    # guarding nothing and the rule has silently moved somewhere unchecked.
    if submissions_in_helper == 0:
        print(
            f"check_worker_dispatch: FAILED (no `RenderTask` in {DISPATCH_HELPER} -- "
            f"the leaf-only dispatch helper is gone or has moved; this lint is vacuous)",
            file=sys.stderr,
        )
        return 1
    print(
        f"check_worker_dispatch: OK (RenderTask submitted only from {DISPATCH_HELPER})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
