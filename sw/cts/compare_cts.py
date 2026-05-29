#!/usr/bin/env python3
"""Fallback CTS answer-tree comparator used by sw/cts/Makefile.

Compares files under two directories by relative path and exact byte content.
Returns 0 when both trees match exactly; otherwise returns 1.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
from pathlib import Path


def file_hash(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def collect_files(root: Path) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            p = Path(dirpath) / name
            rel = str(p.relative_to(root)).replace("\\", "/")
            out[rel] = p
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare two CTS answer directories")
    ap.add_argument("reference", type=Path, help="reference answer tree (RI)")
    ap.add_argument("candidate", type=Path, help="candidate answer tree (cmodel)")
    args = ap.parse_args()

    if not args.reference.is_dir() or not args.candidate.is_dir():
        print("ERROR: both inputs must be directories", file=sys.stderr)
        return 2

    ref_files = collect_files(args.reference)
    cand_files = collect_files(args.candidate)

    ref_set = set(ref_files)
    cand_set = set(cand_files)
    common = sorted(ref_set & cand_set)
    only_ref = sorted(ref_set - cand_set)
    only_cand = sorted(cand_set - ref_set)

    mismatches: list[str] = []
    for rel in common:
        if file_hash(ref_files[rel]) != file_hash(cand_files[rel]):
            mismatches.append(rel)

    print(f"RI files: {len(ref_files)}")
    print(f"cmodel files: {len(cand_files)}")
    print(f"Common files: {len(common)}")
    print(f"Mismatched files: {len(mismatches)}")
    print(f"Only in RI: {len(only_ref)}")
    print(f"Only in cmodel: {len(only_cand)}")

    if mismatches:
        print("\nFirst mismatches:")
        for rel in mismatches[:20]:
            print(f"  {rel}")
    if only_ref:
        print("\nFirst missing in cmodel:")
        for rel in only_ref[:20]:
            print(f"  {rel}")
    if only_cand:
        print("\nFirst extra in cmodel:")
        for rel in only_cand[:20]:
            print(f"  {rel}")

    return 0 if not mismatches and not only_ref and not only_cand else 1


if __name__ == "__main__":
    raise SystemExit(main())
