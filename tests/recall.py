#!/usr/bin/env python3
"""Recall measurement for the approximate mode.

The contest requires an average Recall@10 of at least 0.95 for the filtered
and for the unfiltered query group.  The truth is computed with the exact
mode, the candidate result with the approximate mode.

    python3 tests/recall.py --binary ./minivectordb --nprobe 16
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database  # noqa: E402
from bench import build_dataset, fmt_vec  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=400)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--nprobe", type=int, default=0, help="override the probed clusters")
    ap.add_argument("--categories", type=int, default=16)
    ap.add_argument("--dist", default="uniform", choices=["uniform", "normal", "clustered"])
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    binary = str(Path(args.binary).resolve())
    tmp = Path(tempfile.mkdtemp(prefix="mvdb-recall-"))
    data = tmp / "data"

    rng = random.Random(20261110)
    if args.dist == "uniform":
        rows = build_dataset(args.rows, args.dim, 20261110)
    elif args.dist == "normal":
        rows = []
        for i in range(args.rows):
            vec = [rng.gauss(0.0, 1.0) for _ in range(args.dim)]
            rows.append((i, f"c{rng.randrange(args.categories)}", f"doc-{i}", vec))
    else:  # clustered
        centers = [[rng.uniform(-1, 1) for _ in range(args.dim)] for _ in range(20)]
        rows = []
        for i in range(args.rows):
            c = centers[rng.randrange(len(centers))]
            vec = [max(-1.0, min(1.0, c[d] + rng.gauss(0, 0.1))) for d in range(args.dim)]
            rows.append((i, f"c{rng.randrange(args.categories)}", f"doc-{i}", vec))

    db = Database(binary, data)
    db.start()
    db.request(f"CREATE TABLE items (id BIGINT PRIMARY KEY, category VARCHAR(16), "
               f"content VARCHAR(32), embedding VECTOR({args.dim}))")
    for s in range(0, len(rows), 100):
        chunk = rows[s:s + 100]
        vals = ", ".join(f"({i}, '{c}', '{t}', '{fmt_vec(v)}')" for i, c, t, v in chunk)
        resp = db.request(f"INSERT INTO items VALUES {vals}")
        if resp.get("ok") is not True:
            print("insert failed:", resp, file=sys.stderr)
            return 1
    resp = db.request("CREATE VECTOR INDEX items_vec ON items(embedding)")
    if resp.get("ok") is not True:
        print("index failed:", resp, file=sys.stderr)
        return 1

    qrng = random.Random(args.seed)
    queries = []
    for i in range(args.queries):
        qvec = [qrng.uniform(-1, 1) for _ in range(args.dim)]
        filt = f"category = 'c{qrng.randrange(args.categories)}'" if i % 2 == 0 else ""
        sql = (f"SELECT id, L2_DISTANCE(embedding, '{fmt_vec(qvec)}') AS distance FROM items"
               + (f" WHERE {filt}" if filt else "") + f" ORDER BY distance LIMIT {args.k}")
        queries.append((sql, bool(filt)))

    truth = []
    for sql, _ in queries:
        r = db.request(sql, "exact")
        truth.append([row[0] for row in r.get("rows", [])])

    ann = []
    for sql, _ in queries:
        r = db.request(sql, "ann")
        ann.append([row[0] for row in r.get("rows", [])])

    groups = {"unfiltered": [], "filtered": []}
    for (sql, is_filtered), t, a in zip(queries, truth, ann):
        denom = min(args.k, len(t))
        hit = sum(1 for x in t[:denom] if x in a[:denom])
        groups["filtered" if is_filtered else "unfiltered"].append(hit / denom if denom else 1.0)

    out = {
        "dist": args.dist,
        "rows": args.rows,
        "dim": args.dim,
        "queries": args.queries,
        "nprobe": args.nprobe,
        "recall_unfiltered": round(sum(groups["unfiltered"]) / max(1, len(groups["unfiltered"])), 4),
        "recall_filtered": round(sum(groups["filtered"]) / max(1, len(groups["filtered"])), 4),
        "empty_truth_unfiltered": sum(1 for x in groups["unfiltered"] if x == 0.0),
    }
    print(json.dumps(out))
    db.shutdown()
    if args.keep:
        print(f"kept {tmp}", file=sys.stderr)
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
