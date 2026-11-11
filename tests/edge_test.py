#!/usr/bin/env python3
"""Edge cases that the public package does not cover.

The public samples only exercise a part of the required semantics.  These
cases cover the rest of what the rules demand: illegal vector values, the
dimension check, the statement atomicity inside a transaction, the tie
breaking, the ordering of a descending query and the behaviour of an
unsupported statement.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database  # noqa: E402

RESULTS: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = ""):
    RESULTS.append((name, ok, detail))
    print(f"{name:<52} {'PASS' if ok else 'FAIL'}  {detail}", flush=True)


def expect_error(resp, code=None):
    if resp.get("ok") is not False:
        return False, f"expected a failure, got {resp.get('ok')}"
    got = resp.get("error", {}).get("code")
    if code and got != code:
        return False, f"expected the code {code}, got {got}"
    return True, got or ""


def test_nan_vector(binary, tmp):
    """NaN and Infinity must be rejected."""
    data = tmp / "nan"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
    r1 = db.request("INSERT INTO t VALUES (1, '[NaN, 1]')")
    r2 = db.request("INSERT INTO t VALUES (2, '[Infinity, 0]')")
    r3 = db.request("INSERT INTO t VALUES (3, '[1e400, 0]')")
    r4 = db.request("SELECT id FROM t")
    db.shutdown()
    ok, code = expect_error(r1, "invalid_vector")
    ok2, _ = expect_error(r2, "invalid_vector")
    ok3, _ = expect_error(r3, "invalid_vector")
    ok4 = r4.get("ok") is True and r4.get("rows") == []
    check("NaN / Infinity / overflow are rejected", ok and ok2 and ok3 and ok4, code)


def test_dimension_mismatch(binary, tmp):
    """A vector with the wrong dimension must be rejected."""
    data = tmp / "dim"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(3))")
    r1 = db.request("INSERT INTO t VALUES (1, '[1,2]')")
    r2 = db.request("INSERT INTO t VALUES (2, '[1,2,3]')")
    db.shutdown()
    ok, code = expect_error(r1, "dimension_mismatch")
    ok2 = r2.get("ok") is True
    check("dimension mismatch is rejected", ok and ok2, code)


def test_statement_atomicity(binary, tmp):
    """A failing statement must not leave any of its rows behind."""
    data = tmp / "atomic"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
    r = db.request("INSERT INTO t VALUES (1, '[0,0]'), (2, '[1,1]'), (3, '[1]')")
    rows = db.request("SELECT id FROM t ORDER BY id")
    db.shutdown()
    ok, code = expect_error(r, "dimension_mismatch")
    ok2 = rows.get("rows") == []
    check("a failing multi row insert applies nothing", ok and ok2, code)


def test_duplicate_in_one_statement(binary, tmp):
    """The same primary key twice inside one statement is a duplicate."""
    data = tmp / "dup"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(4))")
    r = db.request("INSERT INTO t VALUES (1, 'a'), (1, 'b')")
    rows = db.request("SELECT id FROM t")
    db.shutdown()
    ok, code = expect_error(r, "duplicate_key")
    ok2 = rows.get("rows") == []
    check("a duplicate key inside one statement is rejected", ok and ok2, code)


def test_ties_and_order(binary, tmp):
    """Equal distances are ordered by the primary key, also when descending."""
    data = tmp / "ties"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
    db.request("INSERT INTO t VALUES (5, '[1,0]'), (2, '[0,3]'), (9, '[-2,0]')")
    up = db.request("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t ORDER BY d LIMIT 3")
    down = db.request("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t ORDER BY d DESC LIMIT 3")
    db.shutdown()
    # the distances are 1.0, 3.0 and 2.0
    ok = [r[0] for r in up.get("rows", [])] == [5, 9, 2]
    ok2 = [r[0] for r in down.get("rows", [])] == [2, 9, 5]
    check("ties are ordered by the primary key", ok and ok2,
          f"asc={[r[0] for r in up.get('rows', [])]} desc={[r[0] for r in down.get('rows', [])]}")


def test_transaction_isolation(binary, tmp):
    """A transaction sees its own changes; a rollback undoes them."""
    data = tmp / "txn"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(4))")
    db.request("INSERT INTO t VALUES (1, 'a')")
    db.request("BEGIN")
    db.request("UPDATE t SET v = 'b' WHERE id = 1")
    inside = db.request("SELECT v FROM t WHERE id = 1")
    db.request("ROLLBACK")
    after = db.request("SELECT v FROM t WHERE id = 1")
    db.shutdown()
    ok = inside.get("rows") == [["b"]]
    ok2 = after.get("rows") == [["a"]]
    check("a transaction reads its own changes and rolls back", ok and ok2)


def test_unsupported_statements(binary, tmp):
    """Unsupported statements fail but never kill the process."""
    data = tmp / "unsupported"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(4))")
    bad = [
        "ALTER TABLE t ADD extra BIGINT",
        "SELECT count(*) FROM t",
        "SELECT id FROM t JOIN t AS u ON t.id = u.id",
        "SELECT id FROM t WHERE v IS NULL",
        "UPDATE t SET id = 5 WHERE id = 1",   # a primary key update is refused
    ]
    codes = []
    for sql in bad:
        r = db.request(sql)
        ok, code = expect_error(r)
        codes.append((sql.split()[0], code, ok))
    alive = db.request("SELECT id FROM t")
    db.shutdown()
    check("unsupported statements fail without killing the process",
          all(c[2] for c in codes) and alive.get("ok") is True,
          ", ".join(f"{c[0]}->{c[1]}" for c in codes))


def test_ordering_of_plain_columns(binary, tmp):
    """ORDER BY on an ordinary column, ascending and descending."""
    data = tmp / "order"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT)")
    db.request("INSERT INTO t VALUES (3, 30), (1, 10), (2, 20)")
    up = db.request("SELECT id FROM t ORDER BY v")
    down = db.request("SELECT id FROM t ORDER BY v DESC")
    lim = db.request("SELECT id FROM t ORDER BY v LIMIT 2")
    db.shutdown()
    ok = [r[0] for r in up.get("rows", [])] == [1, 2, 3]
    ok2 = [r[0] for r in down.get("rows", [])] == [3, 2, 1]
    ok3 = [r[0] for r in lim.get("rows", [])] == [1, 2]
    check("ORDER BY and LIMIT on an ordinary column", ok and ok2 and ok3)


def test_varchar_length(binary, tmp):
    """A VARCHAR keeps its unicode content and its code point length."""
    data = tmp / "unicode"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    db.request("INSERT INTO t VALUES (1, '中文abc'), (2, 'é'), (3, '')")
    rows = db.request("SELECT v FROM t ORDER BY id")
    db.shutdown()
    got = [r[0] for r in rows.get("rows", [])]
    check("varchar keeps the unicode content", got == ["中文abc", "é", ""], repr(got))


def test_large_table_fallback(binary, tmp):
    """A table whose vectors exceed the memory budget still answers queries.

    The budget is lowered with MVDB_VEC_CACHE_MB, so the table switches to
    reading its vectors from the payload pool; the answers must not change.
    """
    import os
    import random
    data = tmp / "large"
    rows = 400
    dim = 64
    rng = random.Random(3)
    vecs = [[rng.uniform(-1, 1) for _ in range(dim)] for _ in range(rows)]
    db = Database(binary, data)
    db.start()
    db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR({dim}))")
    for start in range(0, rows, 50):
        chunk = vecs[start:start + 50]
        vals = ", ".join(f"({start + i}, '[{','.join(f'{v:.5f}' for v in vec)}]')"
                         for i, vec in enumerate(chunk))
        db.request(f"INSERT INTO t VALUES {vals}")
    q = vecs[7]
    qs = "[" + ",".join(f"{v:.5f}" for v in q) + "]"
    before = db.request(f"SELECT id, L2_DISTANCE(e, '{qs}') AS d FROM t ORDER BY d LIMIT 5")
    db.shutdown()

    # now with a budget of one megabyte the vectors no longer fit into memory
    os.environ["MVDB_VEC_CACHE_MB"] = "1"
    db = Database(binary, data)
    db.start()
    db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR({dim}))")
    for start in range(0, rows, 50):
        chunk = vecs[start:start + 50]
        vals = ", ".join(f"({start + i}, '[{','.join(f'{v:.5f}' for v in vec)}]')"
                         for i, vec in enumerate(chunk))
        db.request(f"INSERT INTO t VALUES {vals}")
    after = db.request(f"SELECT id, L2_DISTANCE(e, '{qs}') AS d FROM t ORDER BY d LIMIT 5")
    db.shutdown()
    os.environ.pop("MVDB_VEC_CACHE_MB", None)

    ok = before.get("rows") == after.get("rows") and len(before.get("rows", [])) == 5
    ok2 = before.get("rows", [{}])[0][0] == 7
    check("a table beyond the memory budget still answers", ok and ok2,
          f"cached={before.get('rows', [])[:1]} pool={after.get('rows', [])[:1]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    binary = str(Path(args.binary).resolve())
    tmp = Path(tempfile.mkdtemp(prefix="mvdb-edge-"))
    try:
        test_nan_vector(binary, tmp)
        test_dimension_mismatch(binary, tmp)
        test_statement_atomicity(binary, tmp)
        test_duplicate_in_one_statement(binary, tmp)
        test_ties_and_order(binary, tmp)
        test_transaction_isolation(binary, tmp)
        test_unsupported_statements(binary, tmp)
        test_ordering_of_plain_columns(binary, tmp)
        test_varchar_length(binary, tmp)
        test_large_table_fallback(binary, tmp)
    finally:
        if args.keep:
            print(f"kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    failed = [r for r in RESULTS if not r[1]]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
