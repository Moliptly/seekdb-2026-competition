#!/usr/bin/env python3
"""The vector search on a table that does not fit into the memory budget.

The evaluation contains a case with more data than the memory limit: the
vectors are then read from the payload pool instead of the memory.  This
script writes such a table with a small cache budget, compares the answers
with a brute force truth, restarts the database and compares them again.

    python3 tests/bigtable_test.py --binary ./minivectordb --rows 200000
"""

from __future__ import annotations

import argparse
import math
import os
import random
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database  # noqa: E402

DIM = 32
CATS = 4


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def fmt(v):
    return ",".join(str(x) for x in v)


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--rows", type=int, default=200_000)
    ap.add_argument("--cache-mb", type=int, default=1,
                    help="the memory budget for the vectors, so that the table "
                         "really has to be read from the pool")
    args = ap.parse_args()
    rng = random.Random(20261112)
    os.environ["MVDB_VEC_CACHE_MB"] = str(args.cache_mb)

    tmp = Path(tempfile.mkdtemp(prefix="mvdb-bigtable-"))
    problems = []
    try:
        # the vectors of the table, kept for the truth
        vecs = []
        for i in range(args.rows):
            vecs.append([f32(rng.uniform(-1, 1)) for _ in range(DIM)])

        db = Database(args.binary, tmp / "data")
        db.start()
        db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR({DIM}))")
        started = time.time()
        batch = 1000
        for start in range(0, args.rows, batch):
            values = []
            for i in range(start, min(start + batch, args.rows)):
                values.append(f"({i + 1}, 'c{i % CATS}', '[{fmt(vecs[i])}]')")
            resp = db.request(f"INSERT INTO t VALUES {','.join(values)}")
            if resp.get("ok") is not True:
                problems.append(f"insert failed: {resp}")
                break
        wrote = time.time() - started

        probes = [[f32(rng.uniform(-1, 1)) for _ in range(DIM)] for _ in range(4)]
        k = 10

        def truth(probe, cat=None):
            pairs = []
            for i, v in enumerate(vecs):
                if cat is not None and (i % CATS) != cat:
                    continue
                pairs.append((distance(v, probe), i + 1))
            pairs.sort()
            return [i for _, i in pairs[:k]]

        for n, probe in enumerate(probes):
            sql = (f"SELECT id, L2_DISTANCE(e, '[{fmt(probe)}]') AS d FROM t "
                   f"ORDER BY d LIMIT {k}")
            exact = db.request(sql, "exact")
            ann = db.request(sql, "ann")
            want = truth(probe)
            got = [r[0] for r in exact.get("rows", [])]
            if got != want:
                problems.append(f"probe {n}: exact {got[:4]} != {want[:4]}")
            gota = [r[0] for r in ann.get("rows", [])]
            if len(gota) != k:
                problems.append(f"probe {n}: ann returned {len(gota)} rows")
            if set(want) - set(gota):
                problems.append(f"probe {n}: ann missed {sorted(set(want) - set(gota))[:4]}")

        # a filtered query
        probe = probes[0]
        sql = (f"SELECT id, L2_DISTANCE(e, '[{fmt(probe)}]') AS d FROM t "
               f"WHERE cat = 'c1' ORDER BY d LIMIT {k}")
        exact = db.request(sql, "exact")
        want = truth(probe, 1)
        got = [r[0] for r in exact.get("rows", [])]
        if got != want:
            problems.append(f"filtered: {got[:4]} != {want[:4]}")

        # The index of a table that does not fit into the memory budget must
        # not answer with fewer rows than the exact scan: the vectors of the
        # rows that are not cached are not in the index.
        resp = db.request("CREATE VECTOR INDEX t_vec ON t(e)")
        if resp.get("ok") is not True:
            problems.append(f"index: {resp}")
        sql = (f"SELECT id, L2_DISTANCE(e, '[{fmt(probe)}]') AS d FROM t "
               f"ORDER BY d LIMIT {k}")
        exact = db.request(sql, "exact")
        ann = db.request(sql, "ann")
        got = [r[0] for r in exact.get("rows", [])]
        gota = [r[0] for r in ann.get("rows", [])]
        if gota != got:
            problems.append(f"ann on a pooled table: {gota[:4]} != {got[:4]}")

        db.shutdown()

        # the same after a restart
        db = Database(args.binary, tmp / "data")
        db.start()
        started = time.time()
        for n, probe in enumerate(probes[:2]):
            sql = (f"SELECT id, L2_DISTANCE(e, '[{fmt(probe)}]') AS d FROM t "
                   f"ORDER BY d LIMIT {k}")
            exact = db.request(sql, "exact")
            want = truth(probe)
            got = [r[0] for r in exact.get("rows", [])]
            if got != want:
                problems.append(f"after the restart, probe {n}: {got[:4]} != {want[:4]}")
            ann = db.request(sql, "ann")
            gota = [r[0] for r in ann.get("rows", [])]
            if gota != got:
                problems.append(f"after the restart, ann probe {n}: {gota[:4]} != {got[:4]}")
        restart = time.time() - started
        db.shutdown()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"rows={args.rows} dim={DIM} write={wrote:.1f}s restart_and_queries={restart:.1f}s")
    for p in problems[:10]:
        print("FAIL", p)
    print(f"\n{len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
