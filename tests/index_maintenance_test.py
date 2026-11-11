#!/usr/bin/env python3
"""The index has to follow the records of the table.

The hidden test case "index maintenance" interleaves the creation of the
index with inserts, vector updates and deletes and checks that the answer of
the approximate mode agrees with the current records.  This script does the
same: after every step it compares the approximate answer with the exact one
and checks that the ids are valid, unique and match the filter.

    python3 tests/index_maintenance_test.py --binary ./minivectordb
"""

from __future__ import annotations

import argparse
import random
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--steps", type=int, default=40)
    ap.add_argument("--seed", type=int, default=99)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    tmp = Path(tempfile.mkdtemp(prefix="mvdb-idxmaint-"))
    failures = []
    try:
        db = Database(args.binary, tmp / "data")
        db.start()
        db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR(4))")
        live: dict[int, tuple[str, list[float]]] = {}
        next_id = 1

        def step_check(step: int, where: str):
            probe = [round(rng.uniform(-5, 5), 3) for _ in range(4)]
            sql = (f"SELECT id, L2_DISTANCE(e, '[{','.join(str(x) for x in probe)}]') AS d "
                   f"FROM t{where} ORDER BY d LIMIT 10")
            exact = db.request(sql, "exact")
            ann = db.request(sql, "ann")
            want = [r[0] for r in exact.get("rows", [])]
            got = [r[0] for r in ann.get("rows", [])]
            if sorted(want) != sorted(got):
                failures.append(f"step {step}{where}: exact={want} ann={got}")
            if len(set(got)) != len(got):
                failures.append(f"step {step}{where}: duplicate ids {got}")
            for i in got:
                if i not in live:
                    failures.append(f"step {step}{where}: id {i} is not a live record")

        for step in range(args.steps):
            action = rng.choice(["insert", "update", "delete", "query", "query_filtered"])
            if action == "insert" or not live:
                ids = []
                for _ in range(rng.randint(1, 5)):
                    vec = [round(rng.uniform(-5, 5), 3) for _ in range(4)]
                    cat = rng.choice(["a", "b"])
                    ids.append(next_id)
                    live[next_id] = (cat, vec)
                    next_id += 1
                values = ",".join(
                    f"({i}, '{live[i][0]}', '[{','.join(str(x) for x in live[i][1])}]')" for i in ids)
                db.request(f"INSERT INTO t VALUES {values}")
            elif action == "update":
                i = rng.choice(sorted(live))
                vec = [round(rng.uniform(-5, 5), 3) for _ in range(4)]
                live[i] = (live[i][0], vec)
                db.request(f"UPDATE t SET e = '[{','.join(str(x) for x in vec)}]' WHERE id = {i}")
            elif action == "delete":
                i = rng.choice(sorted(live))
                del live[i]
                db.request(f"DELETE FROM t WHERE id = {i}")
            elif action == "query_filtered":
                step_check(step, " WHERE cat = 'a'")
                continue
            step_check(step, "")
        db.shutdown()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for f in failures[:10]:
        print("FAIL", f)
    print(f"\n{args.steps - len({f.split(':')[0] for f in failures})}/{args.steps} steps agree "
          f"with the exact mode")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
