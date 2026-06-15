#!/usr/bin/env python3
"""CTS answer-tree comparator – reports ALL mismatches with per-group stats."""

from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path
from collections import defaultdict

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

def extract_group(filename: str) -> str:
    """Extract test group letter from filename like 'A10101_Ans.dat'."""
    base = filename.split("/")[-1]
    for i, ch in enumerate(base):
        if ch.isalpha() and i + 1 < len(base) and base[i + 1].isdigit():
            return ch
    return "?"

def extract_test_id(filename: str) -> str:
    """Extract full test ID like 'A10101' from filename."""
    base = filename.split("/")[-1]
    for i, ch in enumerate(base):
        if ch.isalpha() and i + 1 < len(base) and base[i + 1].isdigit():
            j = i + 1
            while j < len(base) and base[j].isdigit():
                j += 1
            return base[i:j]
    return base

def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Compare two CTS answer directories (full report)")
    ap.add_argument("reference", type=Path, help="reference answer tree (RI)")
    ap.add_argument("candidate", type=Path, help="candidate answer tree (cmodel)")
    args = ap.parse_args()

    if not args.reference.is_dir():
        print(f"ERROR: RI answer directory not found: {args.reference}", file=sys.stderr)
        return 2
    if not args.candidate.is_dir():
        print(f"ERROR: cmodel answer directory not found: {args.candidate}", file=sys.stderr)
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

    # Per-group statistics
    group_stats: dict[str, dict] = defaultdict(lambda: {"pass": 0, "fail": 0, "ri_only": 0, "cm_only": 0})

    for rel in common:
        g = extract_group(rel)
        if rel in mismatches:
            group_stats[g]["fail"] += 1
        else:
            group_stats[g]["pass"] += 1

    for rel in only_ref:
        g = extract_group(rel)
        group_stats[g]["ri_only"] += 1

    for rel in only_cand:
        g = extract_group(rel)
        group_stats[g]["cm_only"] += 1

    # Summary
    total_pass = sum(s["pass"] for s in group_stats.values())
    total_fail = sum(s["fail"] for s in group_stats.values())
    total_ri_only = sum(s["ri_only"] for s in group_stats.values())
    total_cm_only = sum(s["cm_only"] for s in group_stats.values())

    print(f"RI files:      {len(ref_files)}")
    print(f"cmodel files:  {len(cand_files)}")
    print(f"Common files:  {len(common)}")
    print(f"Matched:       {total_pass}")
    print(f"Mismatched:    {total_fail}")
    print(f"Only in RI:    {total_ri_only}")
    print(f"Only in cmodel: {total_cm_only}")

    # Per-group table
    if group_stats:
        print(f"\n{'Group':<8} {'Pass':>6} {'Fail':>6} {'RI-only':>8} {'CM-only':>8}")
        print("-" * 40)
        for g in sorted(group_stats.keys()):
            s = group_stats[g]
            print(f"{g:<8} {s['pass']:>6} {s['fail']:>6} {s['ri_only']:>8} {s['cm_only']:>8}")
        print("-" * 40)
        print(f"{'TOTAL':<8} {total_pass:>6} {total_fail:>6} {total_ri_only:>8} {total_cm_only:>8}")

    # List ALL mismatches
    if mismatches:
        print(f"\n=== ALL {len(mismatches)} MISMATCHED FILES ===")
        for i, rel in enumerate(mismatches, 1):
            g = extract_group(rel)
            tid = extract_test_id(rel)
            print(f"  {i:>4}. [{g}] {rel}")

    # List files only in RI
    if only_ref:
        print(f"\n=== {len(only_ref)} FILES ONLY IN RI (missing in cmodel) ===")
        for rel in only_ref:
            print(f"  {rel}")

    # List files only in cmodel
    if only_cand:
        print(f"\n=== {len(only_cand)} FILES ONLY IN CMODEL (extra) ===")
        for rel in only_cand:
            print(f"  {rel}")

    # Overall result
    if not mismatches and not only_ref and not only_cand:
        print("\nRESULT: PASS - all CTS answer files match.")
    else:
        print(f"\nRESULT: FAIL - {total_fail} mismatched, {total_ri_only} RI-only, {total_cm_only} CM-only")

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
