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
import os
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


def test_transaction_vector_visibility(binary, tmp):
    """A transaction sees the vector changes of its own statements, and a
    rollback restores the previous vectors (rules 2.3)."""
    data = tmp / "txnvec"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
    db.request("INSERT INTO t VALUES (1, '[10,0]'), (2, '[0,20]')")
    db.request("BEGIN")
    db.request("UPDATE t SET e = '[0,0]' WHERE id = 1")
    inside = db.request("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t ORDER BY d LIMIT 1")
    db.request("ROLLBACK")
    after = db.request("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t ORDER BY d LIMIT 1")
    db.shutdown()
    check("a transaction sees its own vector changes",
          inside.get("rows") == [[1, 0.0]], repr(inside.get("rows")))
    check("a rollback restores the previous vectors",
          after.get("rows") == [[1, 10.0]], repr(after.get("rows")))


def test_transaction_vector_visibility_ann(binary, tmp):
    """The same in the approximate mode.  The table has enough rows for
    several clusters, so a row whose cluster changed during the transaction
    really is looked for in the wrong cluster after the rollback."""
    data = tmp / "txnvecann"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
    vals = ",".join(f"({i}, '[{i * 10}.0,{(i % 7) * 10}.0]')" for i in range(1, 41))
    db.request(f"INSERT INTO t VALUES {vals}")
    db.request("CREATE VECTOR INDEX t_vec ON t(e)")
    db.request("BEGIN")
    db.request("UPDATE t SET e = '[0,0]' WHERE id = 1")
    inside = db.request("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t "
                        "ORDER BY d LIMIT 1", "ann")
    db.request("ROLLBACK")
    after = db.request("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t "
                       "ORDER BY d LIMIT 5", "ann")
    truth = db.request("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t "
                       "ORDER BY d LIMIT 5")
    db.shutdown()
    check("the approximate mode sees the vector change of a transaction",
          inside.get("rows") == [[1, 0.0]], repr(inside.get("rows")))
    ann_ids = [row[0] for row in after.get("rows", [])]
    exact_ids = [row[0] for row in truth.get("rows", [])]
    check("the approximate mode finds the same rows after a rollback",
          ann_ids == exact_ids, f"ann={ann_ids} exact={exact_ids}")


def test_ann_matches_the_exact_mode(binary, tmp):
    """The approximate mode must return the same rows as the exact mode.

    The test cases compare the answer with fixed expectations, so a missing
    row or a wrong neighbour is a failure even when the recall is high.
    """
    import random
    data = tmp / "annexact"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(4))")
    rng = random.Random(4242)
    vals = []
    for i in range(1, 41):
        v = [round(rng.uniform(-10, 10), 3) for _ in range(4)]
        vals.append(f"({i}, '[{','.join(str(x) for x in v)}]')")
    db.request(f"INSERT INTO t VALUES {','.join(vals)}")
    db.request("CREATE VECTOR INDEX t_vec ON t(e)")
    for k in (1, 3, 10, 40):
        sql = (f"SELECT id, L2_DISTANCE(e, '[1,2,3,4]') AS d FROM t ORDER BY d LIMIT {k}")
        exact = db.request(sql, "exact")
        ann = db.request(sql, "ann")
        if exact.get("rows") != ann.get("rows"):
            db.shutdown()
            check(f"the approximate mode agrees with the exact mode (k={k})", False,
                  f"exact={exact.get('rows')[:3]} ann={ann.get('rows')[:3]}")
            return
    db.shutdown()
    check("the approximate mode agrees with the exact mode", True)


def test_vector_projection(binary, tmp):
    """A projected vector column is a JSON array of the stored values."""
    data = tmp / "vecproj"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(3))")
    db.request("INSERT INTO t VALUES (1, '[1.5,-0.25,2]'), (2, '[0,1,2]')")
    rows = db.request("SELECT id, e FROM t ORDER BY id")
    star = db.request("SELECT * FROM t ORDER BY id")
    db.shutdown()
    check("a projected vector is a JSON array",
          rows.get("rows") == [[1, [1.5, -0.25, 2.0]], [2, [0.0, 1.0, 2.0]]],
          repr(rows.get("rows")))
    check("SELECT * projects the vector as well",
          star.get("rows") == [[1, [1.5, -0.25, 2.0]], [2, [0.0, 1.0, 2.0]]],
          repr(star.get("rows")))


def test_limit_zero(binary, tmp):
    """A LIMIT of zero returns no rows."""
    data = tmp / "limit0"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY)")
    db.request("INSERT INTO t VALUES (1), (2), (3)")
    zero = db.request("SELECT id FROM t LIMIT 0")
    one = db.request("SELECT id FROM t LIMIT 1")
    db.shutdown()
    check("LIMIT 0 returns no rows", zero.get("rows") == [], repr(zero.get("rows")))
    check("LIMIT 1 returns one row", one.get("rows") == [[1]], repr(one.get("rows")))


def test_escaped_quote(binary, tmp):
    """A quote inside a string literal is written twice."""
    data = tmp / "escaped"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, a VARCHAR(32))")
    db.request("INSERT INTO t VALUES (1, 'it''s'), (2, 'O''Brien')")
    rows = db.request("SELECT id, a FROM t ORDER BY id")
    hit = db.request("SELECT id FROM t WHERE a = 'O''Brien'")
    db.shutdown()
    check("a doubled quote is one quote",
          rows.get("rows") == [[1, "it's"], [2, "O'Brien"]], repr(rows.get("rows")))
    check("a doubled quote works in a condition",
          hit.get("rows") == [[2]], repr(hit.get("rows")))


def test_keyword_as_column_name(binary, tmp):
    """A column may be called like a keyword ("vector", "key", "order")."""
    data = tmp / "kwcol"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, vector VECTOR(2), key VARCHAR(8))")
    db.request("INSERT INTO t VALUES (1, '[1,2]', 'k')")
    rows = db.request("SELECT id, vector, key FROM t ORDER BY id")
    dist = db.request("SELECT id, L2_DISTANCE(vector, '[0,0]') AS d FROM t")
    where = db.request("SELECT id FROM t WHERE key = 'k'")
    db.shutdown()
    check("a column may be named like a keyword",
          rows.get("rows") == [[1, [1.0, 2.0], "k"]], repr(rows.get("rows")))
    check("a vector column named like a keyword is searchable",
          dist.get("rows") == [[1, 2.2360680103302]], repr(dist.get("rows")))
    check("a keyword named column works in a condition",
          where.get("rows") == [[1]], repr(where.get("rows")))


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


def test_ann_semantics(binary, tmp):
    """The approximate mode must return valid, unique and matching rows.

    The rules require that the approximate mode returns ids that exist, no
    duplicates, only rows that satisfy the filter and only rows that are
    still visible after the updates and deletes.
    """
    import random
    data = tmp / "ann"
    dim = 16
    rng = random.Random(9)
    n = 500
    vecs = [[rng.uniform(-1, 1) for _ in range(dim)] for _ in range(n)]
    db = Database(binary, data)
    db.start()
    db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR({dim}))")
    for start in range(0, n, 50):
        chunk = vecs[start:start + 50]
        vals = ", ".join(
            f"({start + i}, 'c{(start + i) % 4}', '[{','.join(f'{v:.5f}' for v in vec)}]')"
            for i, vec in enumerate(chunk))
        db.request(f"INSERT INTO t VALUES {vals}")
    db.request("CREATE VECTOR INDEX t_vec ON t(e)")
    # delete a quarter of the rows and move one row to another category
    db.request("DELETE FROM t WHERE id < 100")
    db.request("UPDATE t SET cat = 'c9' WHERE id = 100")
    db.request("INSERT INTO t VALUES (10000, 'c9', '[0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0]')")

    problems = []
    for filtered in (False, True):
        where = " WHERE cat = 'c9'" if filtered else ""
        for _ in range(20):
            q = [rng.uniform(-1, 1) for _ in range(dim)]
            qs = "[" + ",".join(f"{v:.5f}" for v in q) + "]"
            r = db.request(f"SELECT id, L2_DISTANCE(e, '{qs}') AS d FROM t{where} "
                           f"ORDER BY d LIMIT 10", "ann")
            if r.get("ok") is not True:
                problems.append(f"the query failed: {r}")
                continue
            ids = [row[0] for row in r.get("rows", [])]
            if len(ids) != len(set(ids)):
                problems.append(f"duplicate ids: {ids}")
            for i in ids:
                row = db.request(f"SELECT id, cat FROM t WHERE id = {i}")
                if not row.get("rows"):
                    problems.append(f"id {i} does not exist")
                elif filtered and row["rows"][0][1] != "c9":
                    problems.append(f"id {i} does not satisfy the filter")
    db.shutdown()
    check("the approximate mode returns valid, unique and filtered rows",
          not problems, "; ".join(problems[:3]))


def test_ann_no_duplicate_after_vector_update(binary, tmp):
    """A row whose vector is updated after the index exists must not be
    reported twice by the approximate mode."""
    data = tmp / "anndup"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE d (id BIGINT PRIMARY KEY, e VECTOR(4))")
    vals = ",".join(f"({i}, '[{i}.0,0.0,0.0,0.0]')" for i in range(1, 21))
    db.request(f"INSERT INTO d VALUES {vals}")
    db.request("CREATE VECTOR INDEX d_vec ON d(e)")
    db.request("UPDATE d SET e = '[9.0,9.0,9.0,9.0]' WHERE id = 5")
    r = db.request("SELECT id, L2_DISTANCE(e, '[0.0,0.0,0.0,0.0]') AS d FROM d "
                   "ORDER BY d LIMIT 20", "ann")
    db.shutdown()
    ids = [row[0] for row in r.get("rows", [])]
    dups = [i for i in set(ids) if ids.count(i) > 1]
    check("a vector update does not duplicate the row in the index",
          r.get("ok") is True and not dups, f"ids={ids} duplicates={dups}")


def test_writes_only_in_the_data_dir(binary, tmp):
    """The container root is read only, so nothing may be written outside the
    data directory."""
    work = tmp / "ro"
    work.mkdir(parents=True, exist_ok=True)
    data = work / "data"
    cwd = os.getcwd()
    os.chdir(work)
    try:
        db = Database(binary, data)
        db.start()
        db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
        db.request("INSERT INTO t VALUES (1, '[1,2]')")
        db.request("CREATE VECTOR INDEX t_vec ON t(e)")
        db.request("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t")
        db.shutdown()
    finally:
        os.chdir(cwd)
    # the driver's own stderr log is written next to the data directory
    stray = [p.name for p in work.iterdir()
             if p.name != "data" and not p.name.endswith(".stderr.log")]
    inside = sorted(p.name for p in data.iterdir()) if data.exists() else []
    check("nothing is written outside the data directory", not stray,
          f"stray={stray} data={inside}")


def test_large_dimension(binary, tmp):
    """A vector column with more than 1024 dimensions must work."""
    import random
    data = tmp / "bigdim"
    dim = 2048
    rng = random.Random(5)
    vec = [rng.uniform(-1, 1) for _ in range(dim)]
    q = [0.0] * dim
    q[0] = vec[0]
    db = Database(binary, data)
    db.start()
    db.request(f"CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR({dim}))")
    r1 = db.request(f"INSERT INTO t VALUES (1, '[{','.join(f'{v:.5f}' for v in vec)}]')")
    r2 = db.request(f"SELECT id, L2_DISTANCE(e, '[{','.join(f'{v:.5f}' for v in q)}]') AS d "
                    f"FROM t")
    db.shutdown()
    ok = r1.get("ok") is True and r2.get("ok") is True
    ok2 = len(r2.get("rows", [[0, 0]])[0]) == 2
    check("a vector column with 2048 dimensions works", ok and ok2,
          f"insert={r1.get('ok')} rows={r2.get('rows')}")


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
        test_transaction_vector_visibility(binary, tmp)
        test_transaction_vector_visibility_ann(binary, tmp)
        test_ann_matches_the_exact_mode(binary, tmp)
        test_vector_projection(binary, tmp)
        test_limit_zero(binary, tmp)
        test_escaped_quote(binary, tmp)
        test_keyword_as_column_name(binary, tmp)
        test_unsupported_statements(binary, tmp)
        test_ordering_of_plain_columns(binary, tmp)
        test_varchar_length(binary, tmp)
        test_large_table_fallback(binary, tmp)
        test_large_dimension(binary, tmp)
        test_writes_only_in_the_data_dir(binary, tmp)
        test_ann_semantics(binary, tmp)
        test_ann_no_duplicate_after_vector_update(binary, tmp)
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
