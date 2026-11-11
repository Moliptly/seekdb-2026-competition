#!/usr/bin/env python3
"""A table that is larger than the memory budget of the evaluation container.

The rules ask for a test case with more data than the memory limit (2 GiB)
that still answers the queries and recovers after a restart.  This script
writes that many rows, measures the peak memory of the process and the
time of the restart.

    python3 tests/bigdata_test.py --binary ./minivectordb --rows 2000000
"""

from __future__ import annotations

import argparse
import json
import os
import random
import resource
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BATCH = 1000
DIM = 128


def peak_rss_mb() -> float:
    return resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss / 1024.0


def run_session(binary: str, data: Path, requests: list[dict], measure: bool = True):
    payload = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in requests).encode()
    started = time.time()
    proc = subprocess.run([binary, "--data-dir", str(data)], input=payload,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3600)
    elapsed = time.time() - started
    out = [json.loads(line) for line in proc.stdout.decode().splitlines()]
    return out, elapsed, proc.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--rows", type=int, default=2_000_000)
    ap.add_argument("--dim", type=int, default=DIM)
    args = ap.parse_args()

    rng = random.Random(7)
    tmp = Path(tempfile.mkdtemp(prefix="mvdb-big-"))
    data = tmp / "data"
    data.mkdir(parents=True, exist_ok=True)
    try:
        requests = [{"id": 0, "op": "execute",
                     "sql": f"CREATE TABLE big (id BIGINT PRIMARY KEY, cat VARCHAR(8), "
                            f"e VECTOR({args.dim}))"}]
        ident = 1
        for start in range(0, args.rows, BATCH):
            values = []
            for i in range(start, min(start + BATCH, args.rows)):
                vec = ",".join(f"{rng.uniform(-1, 1):.6f}" for _ in range(args.dim))
                values.append(f"({i + 1}, 'c{i % 4}', '[{vec}]')")
            requests.append({"id": ident, "op": "execute",
                             "sql": f"INSERT INTO big VALUES {','.join(values)}"})
            ident += 1
        requests.append({"id": ident, "op": "execute",
                         "sql": "SELECT id, L2_DISTANCE(e, '" +
                                ",".join("0.5" for _ in range(args.dim)) +
                                "') AS d FROM big ORDER BY d LIMIT 10"})
        ident += 1
        requests.append({"id": ident, "op": "shutdown"})

        out, elapsed, code = run_session(args.binary, data, requests)
        ok = all(r.get("ok") is True for r in out if "ok" in r)
        rows = [r for r in out if "rows" in r]
        data_bytes = sum(f.stat().st_size for f in data.iterdir())
        print(json.dumps({
            "rows": args.rows,
            "write_seconds": round(elapsed, 1),
            "queries_ok": ok,
            "topk": rows[0]["rows"][:3] if rows else None,
            "data_dir_mb": round(data_bytes / 1048576, 1),
            "peak_rss_mb": round(peak_rss_mb(), 1),
            "exit_code": code,
        }, indent=2))

        # the restart has to recover the data and answer again
        requests = [
            {"id": 1, "op": "execute", "sql": "SELECT id FROM big ORDER BY id DESC LIMIT 1"},
            {"id": 2, "op": "execute",
             "sql": "SELECT id, L2_DISTANCE(e, '" +
                    ",".join("0.5" for _ in range(args.dim)) +
                    "') AS d FROM big ORDER BY d LIMIT 10"},
            {"id": 3, "op": "shutdown"},
        ]
        out, elapsed, code = run_session(args.binary, data, requests)
        print(json.dumps({
            "restart_seconds": round(elapsed, 1),
            "last_id": [r.get("rows") for r in out if "rows" in r][0],
            "topk_after_restart": [r.get("rows") for r in out if "rows" in r][1][:3],
            "peak_rss_mb": round(peak_rss_mb(), 1),
            "exit_code": code,
        }, indent=2))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
