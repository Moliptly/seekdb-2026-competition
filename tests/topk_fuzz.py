#!/usr/bin/env python3
"""Differential test of the vector search against a brute force truth.

The hidden test cases compute the truth by brute force over the filtered
set and compare the distances, the TopK, the primary keys and the order.
This script does the same: it builds random tables, asks the database for
the TopK with and without a filter and compares the answer with the truth
computed in python.

    python3 tests/topk_fuzz.py --binary ./minivectordb --rounds 200
"""

from __future__ import annotations

import argparse
import math
import random
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database  # noqa: E402


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def truth(rows, probe, k, category=None):
    """The brute force answer: the k nearest rows of the filtered set, the
    distance ties are broken by the primary key."""
    candidates = [r for r in rows if category is None or r[1] == category]
    candidates.sort(key=lambda r: (distance(r[2], probe), r[0]))
    return [(r[0], distance(r[2], probe)) for r in candidates[:k]]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--rounds", type=int, default=200)
    ap.add_argument("--seed", type=int, default=20261111)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    failures = 0
    for round_no in range(args.rounds):
        n = rng.choice([2, 3, 5, 8, 13, 21, 40, 77])
        dim = rng.choice([2, 3, 5, 16])
        categories = ["x", "y", "z"][:rng.choice([1, 2, 3])]
        rows = []
        for i in range(1, n + 1):
            vec = [round(rng.uniform(-5, 5), 3) for _ in range(dim)]
            rows.append((i, rng.choice(categories), vec))
        k = rng.choice([1, 2, 5, 10, n])
        probe = [round(rng.uniform(-5, 5), 3) for _ in range(dim)]
        use_filter = rng.random() < 0.5
        category = rng.choice(categories) if use_filter else None

        tmp = Path(tempfile.mkdtemp(prefix="mvdb-topk-"))
        db = Database(args.binary, tmp / "data")
        db.start()
        db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR({dim}))")
        values = ",".join(
            f"({i}, '{c}', '[{','.join(str(x) for x in v)}]')" for i, c, v in rows)
        db.request(f"INSERT INTO t VALUES {values}")
        where = f" WHERE cat = '{category}'" if category else ""
        sql = (f"SELECT id, L2_DISTANCE(e, '[{','.join(str(x) for x in probe)}]') AS d "
               f"FROM t{where} ORDER BY d LIMIT {k}")
        exact = db.request(sql, "exact")
        ann = db.request(sql, "ann")
        db.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)

        want = truth(rows, probe, k, category)
        got = [(int(r[0]), float(r[1])) for r in exact.get("rows", [])]
        problems = []
        if [g[0] for g in got] != [w[0] for w in want]:
            problems.append(f"the order or the ids differ: got={[g[0] for g in got]} "
                            f"want={[w[0] for w in want]}")
        else:
            for (gi, gd), (wi, wd) in zip(got, want):
                if not math.isclose(gd, wd, abs_tol=1e-5, rel_tol=1e-5):
                    problems.append(f"the distance of id {gi} is {gd}, expected {wd}")
                    break
        ann_rows = [(int(r[0]), float(r[1])) for r in ann.get("rows", [])]
        if [a[0] for a in ann_rows] != [w[0] for w in want]:
            problems.append(f"the approximate mode differs: "
                            f"got={[a[0] for a in ann_rows]} want={[w[0] for w in want]}")
        if problems:
            failures += 1
            if failures <= 5:
                print(f"FAIL round={round_no} n={n} dim={dim} k={k} filter={category}")
                for p in problems:
                    print("   ", p)

    print(f"\n{args.rounds - failures}/{args.rounds} rounds match the brute force truth")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
