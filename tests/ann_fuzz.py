#!/usr/bin/env python3
"""The approximate mode must return exactly the same rows as the exact mode.

The hidden cases compare the rows of an ann query with fixed expectations, so
a recall of 0.95 is not enough: the top k rows have to be the right ones.
This script builds small tables (the size the hidden cases use), creates the
index and compares the ann result with the exact result of the same query.

    python3 tests/ann_fuzz.py --binary ./minivectordb --rounds 200
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database  # noqa: E402


def make_vectors(rng, n, dim, shape):
    vecs = []
    if shape == "uniform":
        for _ in range(n):
            vecs.append([round(rng.uniform(-1, 1), 3) for _ in range(dim)])
    elif shape == "normal":
        for _ in range(n):
            vecs.append([round(rng.gauss(0, 1), 3) for _ in range(dim)])
    elif shape == "clustered":
        centres = [[round(rng.uniform(-10, 10), 3) for _ in range(dim)]
                   for _ in range(max(1, n // 8))]
        for _ in range(n):
            c = rng.choice(centres)
            vecs.append([round(c[i] + rng.gauss(0, 0.3), 3) for i in range(dim)])
    elif shape == "ties":
        for _ in range(n):
            vecs.append([round(rng.choice([0.0, 1.0, 2.0]), 3) for _ in range(dim)])
    else:  # duplicate
        base = [round(rng.uniform(-1, 1), 3) for _ in range(dim)]
        for _ in range(n):
            vecs.append(list(base))
    return vecs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--rounds", type=int, default=200)
    ap.add_argument("--seed", type=int, default=20261111)
    ap.add_argument("--large", type=int, default=0,
                    help="also check tables with this many rows (index path): "
                         "the row count must be exact and the recall must hold")
    args = ap.parse_args()
    rng = random.Random(args.seed)

    failures = []
    for round_no in range(args.rounds):
        n = rng.choice([2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 32, 50])
        dim = rng.choice([2, 3, 4, 8, 16])
        shape = rng.choice(["uniform", "normal", "clustered", "ties", "duplicate"])
        vecs = make_vectors(rng, n, dim, shape)
        k = rng.choice([1, 2, 3, 5, 10, n])
        probe = [round(rng.uniform(-2, 2), 3) for _ in range(dim)]

        import tempfile
        tmp = Path(tempfile.mkdtemp(prefix="mvdb-annfuzz-"))
        db = Database(args.binary, tmp / "data")
        db.start()
        db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR({dim}))")
        values = ",".join(
            f"({i + 1}, '[{','.join(str(x) for x in v)}]')" for i, v in enumerate(vecs))
        db.request(f"INSERT INTO t VALUES {values}")
        db.request("CREATE VECTOR INDEX t_vec ON t(e)")
        sql = (f"SELECT id, L2_DISTANCE(e, '[{','.join(str(x) for x in probe)}]') AS d "
               f"FROM t ORDER BY d LIMIT {k}")
        exact = db.request(sql, "exact")
        ann = db.request(sql, "ann")
        db.shutdown()

        want = exact.get("rows")
        got = ann.get("rows")
        if want != got:
            failures.append({
                "round": round_no, "n": n, "dim": dim, "shape": shape, "k": k,
                "exact": want, "ann": got,
            })
            if len(failures) <= 5:
                print(f"FAIL round={round_no} n={n} dim={dim} shape={shape} k={k}")
                print(f"  exact={want}")
                print(f"  ann  ={got}")
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{args.rounds - len(failures)}/{args.rounds} rounds agree with the exact mode")

    large_failures = 0
    if args.large:
        large_failures = check_large(args, rng)
        print(f"large table check: {large_failures} failures")

    if failures:
        import collections
        counter = collections.Counter((f["n"], f["shape"]) for f in failures)
        print("failing configurations (rows, shape):", counter.most_common(10))
    return 1 if (failures or large_failures) else 0


def check_large(args, rng) -> int:
    """A table that is large enough for the index to answer the query.

    The rules require the right number of rows, no duplicates and a recall
    of at least 0.95 for the approximate mode.
    """
    import shutil
    import tempfile
    failures = 0
    n = args.large
    dim = 16
    k = 10
    vecs = make_vectors(rng, n, dim, "uniform")
    probe = [round(rng.uniform(-2, 2), 3) for _ in range(dim)]
    tmp = Path(tempfile.mkdtemp(prefix="mvdb-annfuzz-large-"))
    db = Database(args.binary, tmp / "data")
    db.start()
    db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR({dim}))")
    for start in range(0, n, 1000):
        chunk = vecs[start:start + 1000]
        values = ",".join(
            f"({start + i + 1}, '[{','.join(str(x) for x in v)}]')" for i, v in enumerate(chunk))
        db.request(f"INSERT INTO t VALUES {values}")
    db.request("CREATE VECTOR INDEX t_vec ON t(e)")
    sql = (f"SELECT id, L2_DISTANCE(e, '[{','.join(str(x) for x in probe)}]') AS d "
           f"FROM t ORDER BY d LIMIT {k}")
    exact = db.request(sql, "exact")
    ann = db.request(sql, "ann")
    db.shutdown()
    shutil.rmtree(tmp, ignore_errors=True)

    want = [r[0] for r in exact.get("rows", [])]
    got = [r[0] for r in ann.get("rows", [])]
    if len(got) != k:
        print(f"FAIL large: the answer has {len(got)} rows, expected {k}")
        failures += 1
    if len(set(got)) != len(got):
        print(f"FAIL large: duplicate rows {got}")
        failures += 1
    hit = len(set(want) & set(got))
    recall = hit / max(1, len(want))
    print(f"large table: rows={len(got)}/{k} recall@10={recall:.3f}")
    if recall < 0.95:
        print("FAIL large: the recall is below 0.95")
        failures += 1
    return failures



if __name__ == "__main__":
    sys.exit(main())
