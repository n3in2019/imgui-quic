#!/usr/bin/env python3
"""Fetch and pin a Dear ImGui revision, regenerate native C bindings, and test.

Usage: python3 tools/bump_upstream.py <revision> [--build-dir build]
Requires Git, Python, CMake, a C++17 compiler, and network access.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMGUI_URL = "https://github.com/ocornut/imgui.git"
IMGUI_DIR = ROOT / "third_party" / "imgui"

# Exactly one revision pin per file; the portfile pattern targets the
# vcpkg_from_git block for ocornut/imgui, not the port's own source REF.
PIN_FILES = [
    (ROOT / "cmake" / "Dependencies.cmake", r'(set\(IMGUI_REVISION ")[0-9a-f]{40}(")'),
    (ROOT / "tools" / "dear_bindings" / "generate.py", r'(IMGUI_REVISION = ")[0-9a-f]{40}(")'),
    (ROOT / "ports" / "imgui-quic" / "portfile.cmake",
     r'(ocornut/imgui\.git"\s*\n\s*REF ")[0-9a-f]{40}(")'),
]


def run(*args: object) -> None:
    print(f"+ {' '.join(str(a) for a in args)}", flush=True)
    subprocess.run([str(a) for a in args], check=True)


def resolve_revision(rev: str) -> str:
    """Resolve a tag/branch name to its full commit SHA (SHAs pass through)."""
    if re.fullmatch(r"[0-9a-f]{40}", rev):
        return rev
    ls = subprocess.run(["git", "ls-remote", IMGUI_URL], capture_output=True, text=True,
                        check=True).stdout
    shas = set()
    for line in ls.splitlines():
        sha, _, ref = line.partition("\t")
        if ref.endswith("^{}"):
            continue  # peeled-tag duplicates
        if ref == rev or ref.endswith("/" + rev):
            shas.add(sha)
    if len(shas) != 1:
        candidates = "\n  ".join(sorted(
            line.split("\t")[1] for line in ls.splitlines() if rev in line))
        sys.exit(f"revision '{rev}' matched {len(shas)} refs. Candidates:\n  {candidates or '(none)'}")
    return shas.pop()


def update_checkout(sha: str) -> None:
    """Move third_party/imgui to the SHA; CMake's fetch only runs when missing."""
    if not (IMGUI_DIR / "imgui.h").exists():
        sys.exit(f"Dear ImGui not fetched at {IMGUI_DIR}; run 'cmake -B build' once first.")
    run("git", "-C", IMGUI_DIR, "fetch", "--depth", "1", IMGUI_URL, sha)
    run("git", "-C", IMGUI_DIR, "checkout", "--detach", "FETCH_HEAD")


def re_pin(sha: str, pattern: str, path: Path, label: str) -> None:
    text = path.read_text()
    new_text, count = re.subn(pattern, r"\g<1>" + sha + r"\g<2>", text)
    if count != 1:
        sys.exit(f"expected exactly one {label} pin in {path}, found {count}")
    path.write_text(new_text)
    print(f"pinned {path.relative_to(ROOT)}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("revision", help="ocornut/imgui tag, branch, or 40-char SHA")
    parser.add_argument("--dear-bindings-rev",
                        help="also re-pin DEAR_BINDINGS_REVISION in generate.py")
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()

    sha = resolve_revision(args.revision)
    print(f"Bumping Dear ImGui -> {sha}")
    update_checkout(sha)
    for path, pattern in PIN_FILES:
        re_pin(sha, pattern, path, "IMGUI_REVISION")
    if args.dear_bindings_rev:
        re_pin(args.dear_bindings_rev, r'(DEAR_BINDINGS_REVISION = ")[0-9a-f]{40}(")',
               ROOT / "tools" / "dear_bindings" / "generate.py", "DEAR_BINDINGS_REVISION")

    run(sys.executable, ROOT / "tools" / "dear_bindings" / "generate.py")

    run("cmake", "-S", ROOT, "-B", ROOT / args.build_dir,
        "-DIMGUI_QUIC_BUILD_TESTS=ON", "-DIMGUI_QUIC_BUILD_EXAMPLES=ON")
    run("cmake", "--build", ROOT / args.build_dir)
    run("ctest", "--test-dir", ROOT / args.build_dir, "--output-on-failure")
    print("Upstream bump complete. Review the three pins, native C bindings and metadata. "
          "At release time refresh the port version and registry git-tree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
