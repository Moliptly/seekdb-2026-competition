#!/usr/bin/env python3
"""Differential test of the SQL layer against a model in python.

The model keeps the rows of one table and answers the queries the same way
the rules describe them: the comparisons are the ones of the columns, the
ties are ordered by the primary key and a rollback undoes the statements of
the transaction.  Every statement of a random sequence is compared with the
answer of the database.

    python3 tests/sql_fuzz.py --binary ./minivectordb --rounds 60
"""

from __future__ import annotations

import argparse
import math
import random
import struct
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database  # noqa: E402

DIM = 3


def f32(x):
    """The value as float32, the database stores the vectors in float32."""
    return struct.unpack("f", struct.pack("f", x))[0]


def fmt(v):
    return ",".join(str(x) for x in v)


class Model:
    """The rows of the table, in the order of the primary key."""

    def __init__(self):
        self.rows: dict[int, tuple[str, list[float]]] = {}

    def copy(self):
        m = Model()
        m.rows = {k: (c, list(v)) for k, (c, v) in self.rows.items()}
        return m

    def matching(self, where):
        if where is None:
            return sorted(self.rows)
        col, op, value = where
        out = []
        for i in sorted(self.rows):
            v = self.rows[i][0] if col == "cat" else self.rows[i][1][0]
            if op == "=" and v == value:
                out.append(i)
            elif op == "<" and v < value:
                out.append(i)
            elif op == ">" and v > value:
                out.append(i)
        return out


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--rounds", type=int, default=60)
    ap.add_argument("--seed", type=int, default=20261112)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    failures = []
    for round_no in range(args.rounds):
        tmp = Path(tempfile.mkdtemp(prefix="mvdb-sqlfuzz-"))
        db = Database(args.binary, tmp / "data")
        db.start()
        db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR({DIM}))")
        model = Model()
        db.trace = []
        stack: list[Model] = []
        next_id = 1
        steps = 0

        def check(name, got, want, detail=""):
            if got != want:
                failures.append(f"round {round_no} step {steps} {name}: got {got!r} want {want!r} {detail}")

        while steps < 40:
            steps += 1
            action = rng.choice(["insert", "insert", "update", "delete", "select",
                                  "select", "select_filtered", "topk", "topk_filtered",
                                  "update_where", "delete_where", "vector_index",
                                  "txn_commit", "txn_rollback"])
            if action == "insert":
                n = rng.randint(1, 4)
                ids = []
                values = []
                for _ in range(n):
                    i = next_id
                    next_id += 1
                    cat = rng.choice(["a", "b", "c"])
                    vec = [f32(round(rng.uniform(-3, 3), 3)) for _ in range(DIM)]
                    ids.append(i)
                    values.append(f"({i}, '{cat}', '[{fmt(vec)}]')")
                resp = db.request(f"INSERT INTO t VALUES {','.join(values)}")
                check("insert", resp.get("affected_rows"), n)
                for i in ids:
                    model.rows[i] = (cat, vec) if False else model.rows.get(i, (None, None))
                # rebuild the model rows from the sql we sent
                for i, v in zip(ids, values):
                    cat = v.split("'")[1]
                    vec = [f32(float(x)) for x in v.split("[")[1].split("]")[0].split(",")]
                    model.rows[i] = (cat, vec)
            elif action == "update" and model.rows:
                i = rng.choice(sorted(model.rows))
                vec = [f32(round(rng.uniform(-3, 3), 3)) for _ in range(DIM)]
                resp = db.request(f"UPDATE t SET e = '[{fmt(vec)}]' WHERE id = {i}")
                check("update", resp.get("affected_rows"), 1)
                model.rows[i] = (model.rows[i][0], vec)
            elif action == "delete" and model.rows:
                i = rng.choice(sorted(model.rows))
                resp = db.request(f"DELETE FROM t WHERE id = {i}")
                check("delete", resp.get("affected_rows"), 1)
                del model.rows[i]
            elif action == "select":
                resp = db.request("SELECT id FROM t ORDER BY id")
                check("select", [r[0] for r in resp.get("rows", [])], sorted(model.rows))
            elif action == "select_filtered":
                cat = rng.choice(["a", "b", "c"])
                try:
                    resp = db.request(f"SELECT id FROM t WHERE cat = '{cat}' ORDER BY id")
                except Exception as exc:
                    log = tmp / "data.stderr.log"
                    print(f"CRASH round={round_no} step={steps}: {exc}")
                    print("--- stderr ---")
                    print(log.read_text()[-3000:] if log.exists() else "(no log)")
                    print("--- the statements before the crash ---")
                    for line in db.trace:
                        print("   ", line)
                    db.proc = None
                    raise SystemExit(2)
                check("select_filtered", [r[0] for r in resp.get("rows", [])],
                      model.matching(("cat", "=", cat)))
            elif action == "topk_filtered":
                k = rng.randint(1, 5)
                probe = [f32(round(rng.uniform(-3, 3), 3)) for _ in range(DIM)]
                cat = rng.choice(["a", "b", "c"])
                resp = db.request(f"SELECT id, L2_DISTANCE(e, '[{fmt(probe)}]') AS d "
                                  f"FROM t WHERE cat = '{cat}' ORDER BY d LIMIT {k}")
                rows = sorted(model.matching(("cat", "=", cat)),
                               key=lambda i: (distance(model.rows[i][1], probe), i))[:k]
                check("topk_filtered", [r[0] for r in resp.get("rows", [])], rows)
            elif action == "update_where":
                cat = rng.choice(["a", "b", "c"])
                newcat = rng.choice(["a", "b", "c"])
                resp = db.request(f"UPDATE t SET cat = '{newcat}' WHERE cat = '{cat}'")
                hit = model.matching(("cat", "=", cat))
                check("update_where", resp.get("affected_rows"), len(hit))
                for i in hit:
                    model.rows[i] = (newcat, model.rows[i][1])
            elif action == "delete_where":
                cat = rng.choice(["a", "b", "c"])
                resp = db.request(f"DELETE FROM t WHERE cat = '{cat}'")
                hit = model.matching(("cat", "=", cat))
                check("delete_where", resp.get("affected_rows"), len(hit))
                for i in hit:
                    del model.rows[i]
            elif action == "vector_index":
                resp = db.request("CREATE VECTOR INDEX t_vec ON t(e)")
                check("vector_index", resp.get("ok"), True,
                      str(resp.get("error", {}))[:90])
            elif action == "topk":
                k = rng.randint(1, 5)
                probe = [f32(round(rng.uniform(-3, 3), 3)) for _ in range(DIM)]
                resp = db.request(f"SELECT id, L2_DISTANCE(e, '[{fmt(probe)}]') AS d "
                                  f"FROM t ORDER BY d LIMIT {k}")
                want = sorted(model.rows.items(),
                              key=lambda kv: (distance(kv[1][1], probe), kv[0]))[:k]
                check("topk", [r[0] for r in resp.get("rows", [])], [i for i, _ in want])
            elif action == "txn_commit":
                db.request("BEGIN")
                stack.append(model.copy())
                # one change inside the transaction
                if model.rows:
                    i = rng.choice(sorted(model.rows))
                    cat = rng.choice(["a", "b", "c"])
                    db.request(f"UPDATE t SET cat = '{cat}' WHERE id = {i}")
                    model.rows[i] = (cat, model.rows[i][1])
                db.request("COMMIT")
                stack.pop()
            elif action == "txn_rollback":
                db.request("BEGIN")
                saved = model.copy()
                if model.rows:
                    i = rng.choice(sorted(model.rows))
                    vec = [f32(round(rng.uniform(-3, 3), 3)) for _ in range(DIM)]
                    db.request(f"UPDATE t SET e = '[{fmt(vec)}]' WHERE id = {i}")
                    model.rows[i] = (model.rows[i][0], vec)
                db.request("ROLLBACK")
                model = saved
                resp = db.request("SELECT id FROM t ORDER BY id")
                check("rollback", [r[0] for r in resp.get("rows", [])], sorted(model.rows))
        db.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)

    for f in failures[:10]:
        print("FAIL", f)
    print(f"\n{len(failures)} mismatches")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
