#!/usr/bin/env python3
"""Performance benchmark that mirrors the contest measurement.

  * 10 000 records with 128 dimensional vectors,
  * the write phase inserts the records in batches of 100 and commits each
    batch,
  * the query phase runs 1 000 TopK queries with K = 10, half of them with an
    equality filter on an ordinary column.

It reports the three quantities the contest ranks on:

  T   wall clock time of the write and of the query phase,
  B   bytes written to the data device (read from /proc/<pid>/io),
  M   peak memory of the database process (VmHWM).

    python3 tests/bench.py --binary ./minivectordb --mode ann
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database  # noqa: E402

NROWS = 10000
DIM = 128
K = 10
NQUERIES = 1000
BATCH = 100
CATEGORIES = 16


def read_proc(pid: int, name: str) -> int:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith(name):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


def read_io(pid: int) -> dict:
    out = {}
    try:
        for line in Path(f"/proc/{pid}/io").read_text().splitlines():
            k, _, v = line.partition(":")
            out[k.strip()] = int(v.strip())
    except OSError:
        pass
    return out


def build_dataset(n: int, dim: int, seed: int = 20261110):
    rng = random.Random(seed)
    rows = []
    for i in range(n):
        vec = [rng.uniform(-1.0, 1.0) for _ in range(dim)]
        rows.append((i, f"c{rng.randrange(CATEGORIES)}", f"doc-{i}", vec))
    return rows


def fmt_vec(vec):
    return "[" + ",".join(f"{v:.6f}" for v in vec) + "]"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--rows", type=int, default=NROWS)
    ap.add_argument("--dim", type=int, default=DIM)
    ap.add_argument("--queries", type=int, default=NQUERIES)
    ap.add_argument("--batches", type=int, default=0, help="rows per batch (default 100)")
    ap.add_argument("--mode", default="ann", choices=["ann", "exact"])
    ap.add_argument("--index", action="store_true", help="create a vector index before the queries")
    ap.add_argument("--json", default="", help="write the result as JSON")
    ap.add_argument("--dist", default="uniform", choices=["uniform", "normal", "clustered"])
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    batch = args.batches or BATCH
    binary = str(Path(args.binary).resolve())
    tmp = Path(tempfile.mkdtemp(prefix="mvdb-bench-"))
    data = tmp / "data"
    if args.dist == "uniform":
        rows = build_dataset(args.rows, args.dim)
    else:
        rng0 = random.Random(20261110)
        rows = []
        if args.dist == "normal":
            for i in range(args.rows):
                vec = [rng0.gauss(0.0, 1.0) for _ in range(args.dim)]
                rows.append((i, f"c{rng0.randrange(CATEGORIES)}", f"doc-{i}", vec))
        else:
            centers = [[rng0.uniform(-1, 1) for _ in range(args.dim)] for _ in range(20)]
            for i in range(args.rows):
                c = centers[rng0.randrange(len(centers))]
                vec = [max(-1.0, min(1.0, c[d] + rng0.gauss(0, 0.1))) for d in range(args.dim)]
                rows.append((i, f"c{rng0.randrange(CATEGORIES)}", f"doc-{i}", vec))

    os.environ["MVDB_TIMING"] = "1"
    db = Database(binary, data)
    db.start()
    pid = db.proc.pid

    t0 = time.monotonic()
    db.request("CREATE TABLE items (id BIGINT PRIMARY KEY, category VARCHAR(16), "
               "content VARCHAR(32), embedding VECTOR(%d))" % args.dim)
    for start in range(0, len(rows), batch):
        chunk = rows[start:start + batch]
        values = ", ".join(
            f"({i}, '{cat}', '{txt}', '{fmt_vec(vec)}')" for i, cat, txt, vec in chunk)
        resp = db.request(f"INSERT INTO items VALUES {values}")
        if resp.get("ok") is not True:
            print("insert failed:", resp, file=sys.stderr)
            return 1
    t_write = time.monotonic() - t0

    io_after_write = read_io(pid)

    if args.index:
        t0 = time.monotonic()
        resp = db.request("CREATE VECTOR INDEX items_vec ON items(embedding)")
        if resp.get("ok") is not True:
            print("index failed:", resp, file=sys.stderr)
            return 1
        t_index = time.monotonic() - t0
    else:
        t_index = 0.0

    rng = random.Random(7)
    queries = []
    for i in range(args.queries):
        qvec = [rng.uniform(-1.0, 1.0) for _ in range(args.dim)]
        filt = f"category = 'c{rng.randrange(CATEGORIES)}'" if i % 2 == 0 else ""
        sql = (f"SELECT id, L2_DISTANCE(embedding, '{fmt_vec(qvec)}') AS distance FROM items"
               + (f" WHERE {filt}" if filt else "")
               + f" ORDER BY distance LIMIT {K}")
        queries.append(sql)

    t0 = time.monotonic()
    for sql in queries:
        resp = db.request(sql, args.mode)
        if resp.get("ok") is not True:
            print("query failed:", resp, file=sys.stderr)
            return 1
    t_query = time.monotonic() - t0

    io = read_io(pid)
    peak_rss_kb = read_proc(pid, "VmHWM:")
    db.shutdown()

    # the server reports its own parse/execute times on stderr; the driver
    # overhead (python) is not part of the database cost
    server_ms = {"parse": 0.0, "exec": 0.0, "n": 0}
    try:
        for line in db.stderr_path.read_text(errors="replace").splitlines():
            if "[mvdb] parse" in line:
                parts = line.split()
                server_ms["parse"] += float(parts[2])
                server_ms["exec"] += float(parts[5])
                server_ms["n"] += 1
    except OSError:
        pass

    write_bytes = io.get("write_bytes", 0)
    read_bytes = io.get("rbytes", 0)
    result = {
        "rows": len(rows),
        "dim": args.dim,
        "queries": args.queries,
        "mode": args.mode,
        "index": bool(args.index),
        "write_seconds": round(t_write, 4),
        "index_seconds": round(t_index, 4),
        "query_seconds": round(t_query, 4),
        "write_rows_per_second": round(len(rows) / t_write, 1),
        "queries_per_second": round(args.queries / t_query, 1),
        "query_us": round(t_query / args.queries * 1e6, 1),
        "bytes_written": write_bytes,
        "bytes_read": read_bytes,
        "bytes_written_after_load": io.get("write_bytes", 0) - io_after_write.get("write_bytes", 0),
        "server_parse_ms": round(server_ms["parse"], 2),
        "server_exec_ms": round(server_ms["exec"], 2),
        "server_total_ms": round(server_ms["parse"] + server_ms["exec"], 2),
        "server_statements": server_ms["n"],
        "peak_rss_kb": peak_rss_kb,
        "peak_rss_mb": round(peak_rss_kb / 1024, 1),
        "data_dir_bytes": sum(f.stat().st_size for f in data.glob("*") if f.is_file()),
    }
    print(json.dumps(result, indent=2))
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2) + "\n")
    if args.keep:
        print(f"kept {tmp}")
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
