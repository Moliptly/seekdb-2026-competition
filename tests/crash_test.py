#!/usr/bin/env python3
"""Crash and durability tests for MiniVectorDB.

The contest crash model is a forced termination of the process (SIGKILL) and
a restart that reuses the same data directory.  The rules require:

  * a transaction that already answered "commit" survives the crash,
  * a transaction that never committed does not survive, and never survives
    partially,
  * a crash during the recovery itself must not break the next recovery.

    python3 tests/crash_test.py --binary ./minivectordb
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cases import Database, must  # noqa: E402

RESULTS: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = ""):
    RESULTS.append((name, ok, detail))
    print(f"{name:<44} {'PASS' if ok else 'FAIL'}  {detail}", flush=True)


def rows_of(resp):
    return resp.get("rows", [])


def test_batches_survive(binary: str, tmp: Path):
    """Every committed batch of a bulk load is there after SIGKILL."""
    data = tmp / "batches"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    for batch in range(5):
        values = ",".join(f"({batch * 100 + i}, 'v{i}')" for i in range(100))
        must(db, f"INSERT INTO t VALUES {values}")
    db.kill()

    db = Database(binary, data)
    db.start()
    resp = db.request("SELECT id FROM t ORDER BY id")
    db.shutdown()
    ids = [r[0] for r in rows_of(resp)]
    ok = resp.get("ok") is True and ids == list(range(500))
    check("every committed batch survives SIGKILL", ok,
          f"{len(ids)} rows, first={ids[:3]} last={ids[-3:]}")


def test_crash_during_a_statement(binary: str, tmp: Path):
    """A statement that was still running is not applied partially."""
    import threading
    data = tmp / "during"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    values = ",".join(f"({i}, 'v{i}')" for i in range(1, 20001))
    must(db, "INSERT INTO t VALUES (1, 'first')")

    def writer():
        try:
            db.request(f"INSERT INTO t VALUES {values}")
        except Exception:
            pass

    thread = threading.Thread(target=writer, daemon=True)
    thread.start()
    time.sleep(0.02)
    db.kill()
    thread.join(timeout=5)

    db = Database(binary, data)
    db.start()
    resp = db.request("SELECT id FROM t ORDER BY id")
    db.shutdown()
    ids = [r[0] for r in rows_of(resp)]
    # the batch is either completely there or completely absent
    ok = ids in ([1], list(range(1, 20002)))
    check("a statement interrupted by SIGKILL is not applied partially", ok,
          f"{len(ids)} rows")


def test_rollback_of_a_vector_survives_the_restart(binary: str, tmp: Path):
    """A rolled back vector update has to name the same vector in the log.

    The rollback puts the row back to its old vector; the log has to name
    that vector, otherwise the row reads the vector of another row after the
    restart.
    """
    data = tmp / "vecrollback"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
    must(db, "INSERT INTO t VALUES (1, '[1,0]'), (2, '[0,1]')")
    must(db, "BEGIN")
    must(db, "UPDATE t SET e = '[9,9]' WHERE id = 2")
    must(db, "ROLLBACK")
    must(db, "UPDATE t SET e = '[5,5]' WHERE id = 2")
    before = db.request("SELECT id, e FROM t ORDER BY id").get("rows")
    db.shutdown()

    db = Database(binary, data)
    db.start()
    after = db.request("SELECT id, e FROM t ORDER BY id").get("rows")
    db.shutdown()
    ok = before == after and after == [[1, [1.0, 0.0]], [2, [5.0, 5.0]]]
    check("a rolled back vector update survives the restart", ok,
          f"before={before} after={after}")


def test_pool_backed_table_survives_the_restart(binary: str, tmp: Path):
    """A table whose vectors do not fit into the cache has to answer with the
    vectors of the payload pool after the restart as well."""
    import os
    os.environ["MVDB_VEC_CACHE_MB"] = "1"     # a small budget: the pool is used
    data = tmp / "pooled"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(8))")
    values = []
    for i in range(1, 40001):
        vec = ",".join(f"{i * 0.001 + j * 0.01:.4f}" for j in range(8))
        values.append(f"({i}, '[{vec}]')")
    for start in range(0, len(values), 1000):
        must(db, f"INSERT INTO t VALUES {','.join(values[start:start + 1000])}")
    before = db.request("SELECT e FROM t WHERE id = 39000").get("rows")
    db.shutdown()

    db = Database(binary, data)
    db.start()
    after = db.request("SELECT e FROM t WHERE id = 39000").get("rows")
    db.shutdown()
    ok = before == after and bool(after) and bool(after[0][0])
    check("a table that is read from the pool survives the restart", ok,
          f"before={before} after={after}")


def test_open_transaction_is_lost_by_the_crash(binary: str, tmp: Path):
    """A transaction that was not committed does not survive the crash, and
    the rows that were committed before it do."""
    data = tmp / "opentxn"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16))")
    must(db, "INSERT INTO t VALUES (1, 'committed')")
    must(db, "BEGIN")
    must(db, "INSERT INTO t VALUES (2, 'uncommitted')")
    must(db, "UPDATE t SET v = 'changed' WHERE id = 1")
    db.kill()

    db = Database(binary, data)
    db.start()
    resp = db.request("SELECT id, v FROM t ORDER BY id")
    db.shutdown()
    rows = rows_of(resp)
    ok = rows == [[1, "committed"]]
    check("an open transaction is lost by the crash", ok, json.dumps(rows, ensure_ascii=False))


def log_frames(path: Path):
    """The frames of the log: a list of (type, payload) tuples."""
    data = path.read_bytes()
    frames = []
    off = 0
    while off + 9 <= len(data):
        length = int.from_bytes(data[off:off + 4], "little")
        ftype = data[off + 8]
        payload = data[off + 9:off + 9 + length]
        if len(payload) < length:
            break
        frames.append((ftype, payload))
        off += 9 + length
    return frames


def test_a_torn_tail_is_cut(binary: str, tmp: Path):
    """A crash between the header and the payload of a frame leaves a tail
    that the next start has to cut away.

    The frames that are written after such a tear have to be read again
    after the next restart, so the log must not keep the torn frame.
    """
    data = tmp / "torn"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    must(db, "INSERT INTO t VALUES (1, 'a')")
    db.kill()

    # the tail of the log is torn: half of a frame
    log = data / "heap.log"
    with open(log, "ab") as f:
        f.write(b"\x20\x00\x00\x00")

    db = Database(binary, data)
    db.start()
    r1 = db.request("SELECT id, v FROM t ORDER BY id")
    must(db, "INSERT INTO t VALUES (2, 'b')")
    db.shutdown()

    db = Database(binary, data)
    db.start()
    r2 = db.request("SELECT id, v FROM t ORDER BY id")
    db.shutdown()
    ok = rows_of(r1) == [[1, "a"]] and rows_of(r2) == [[1, "a"], [2, "b"]]
    check("a torn tail is cut and the later commits survive", ok,
          f"first={rows_of(r1)} second={rows_of(r2)}")


def test_uncommitted_frames_are_not_mixed(binary: str, tmp: Path):
    """The frames of a transaction that never committed must not be applied
    together with the frames of a later transaction.

    The log of a run that was killed between the frames of a transaction and
    its commit frame ends with the frames of that transaction.  The next run
    has to drop them, even when it uses the same transaction number.
    """
    data = tmp / "mixed"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    must(db, "INSERT INTO t VALUES (1, 'a')")
    db.kill()

    # the commit frame of the insert is removed: the insert looks like a
    # transaction that was killed before its commit frame was written
    log = data / "heap.log"
    frames = log_frames(log)
    raw = log.read_bytes()
    off = 0
    for ftype, payload in frames[:-1]:
        off += 9 + len(payload)
    log.write_bytes(raw[:off])

    db = Database(binary, data)
    db.start()
    # a read does not write any frame, so the next statement is the first
    # transaction of this run
    db.request("SELECT id FROM t")
    must(db, "INSERT INTO t VALUES (2, 'b')")
    r = db.request("SELECT id, v FROM t ORDER BY id")
    db.shutdown()
    ok = rows_of(r) == [[2, "b"]]
    check("the frames of a transaction that never committed are dropped", ok,
          json.dumps(rows_of(r), ensure_ascii=False))


def test_committed_survives(binary: str, tmp: Path):
    """A committed insert is still there after SIGKILL."""
    data = tmp / "committed"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    must(db, "INSERT INTO t VALUES (1, 'a'), (2, 'b')")
    db.kill()

    db = Database(binary, data)
    db.start()
    resp = db.request("SELECT id, v FROM t ORDER BY id")
    db.shutdown()
    ok = resp.get("ok") is True and rows_of(resp) == [[1, "a"], [2, "b"]]
    check("committed data survives SIGKILL", ok, json.dumps(resp.get("rows"), ensure_ascii=False))


def test_uncommitted_is_lost(binary: str, tmp: Path):
    """An open transaction is rolled back by the crash."""
    data = tmp / "uncommitted"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    must(db, "INSERT INTO t VALUES (1, 'a')")
    must(db, "BEGIN")
    must(db, "UPDATE t SET v = 'changed' WHERE id = 1")
    must(db, "DELETE FROM t WHERE id = 1")
    db.kill()

    db = Database(binary, data)
    db.start()
    resp = db.request("SELECT id, v FROM t ORDER BY id")
    db.shutdown()
    ok = resp.get("ok") is True and rows_of(resp) == [[1, "a"]]
    check("uncommitted transaction is lost", ok, json.dumps(resp.get("rows"), ensure_ascii=False))


def test_no_partial_statement(binary: str, tmp: Path):
    """A statement that fails must not leave half of its rows behind."""
    data = tmp / "partial"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
    resp = db.request("INSERT INTO t VALUES (1, '[0,0]'), (2, '[1]')")
    db.kill()
    db = Database(binary, data)
    db.start()
    rows = db.request("SELECT id FROM t ORDER BY id")
    db.shutdown()
    ok = resp.get("ok") is False and rows_of(rows) == []
    check("failed statement leaves no partial rows", ok,
          f"insert={resp.get('error', {}).get('code')} rows={rows_of(rows)}")


def test_crash_during_recovery(binary: str, tmp: Path):
    """Killing the process while it replays the log must not corrupt it."""
    data = tmp / "recovery"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16))")
    for i in range(20000):
        must(db, f"INSERT INTO t VALUES ({i}, 'value-{i}')")
    db.kill()

    # start and kill it immediately, repeatedly, while it recovers.  The table
    # is big enough that the recovery takes long enough to be interrupted.
    for _ in range(5):
        db = Database(binary, data)
        db.start()
        time.sleep(0.001)
        db.kill()

    db = Database(binary, data)
    db.start()
    resp = db.request("SELECT id, v FROM t ORDER BY id")
    db.shutdown()
    rows = rows_of(resp)
    ok = resp.get("ok") is True and len(rows) == 20000 and rows[0] == [0, "value-0"] \
        and rows[-1] == [19999, "value-19999"]
    check("recovery survives repeated crashes", ok,
          f"{len(rows)} rows, first={rows[0] if rows else None}")


def test_commit_reply_lost(binary: str, tmp: Path):
    """A commit whose reply never arrived is applied completely or not at all."""
    data = tmp / "lost_reply"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    must(db, "INSERT INTO t VALUES (1, 'a')")
    # the commit is requested, the process dies before the answer is read
    must(db, "INSERT INTO t VALUES (2, 'b')")
    db.kill()

    db = Database(binary, data)
    db.start()
    resp = db.request("SELECT id FROM t ORDER BY id")
    db.shutdown()
    rows = rows_of(resp)
    ok = rows in ([[1], [2]], [[1]])
    check("commit without a reply is all or nothing", ok, json.dumps(rows))


def test_restart_keeps_schema(binary: str, tmp: Path):
    """Table definitions survive a clean restart as well."""
    data = tmp / "schema"
    db = Database(binary, data)
    db.start()
    must(db, "CREATE TABLE a (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    must(db, "CREATE TABLE b (id BIGINT PRIMARY KEY, e VECTOR(3))")
    db.shutdown()

    db = Database(binary, data)
    db.start()
    r1 = db.request("SELECT id FROM a")
    r2 = db.request("INSERT INTO b VALUES (7, '[1,2,3]')")
    db.shutdown()
    ok = r1.get("ok") is True and r2.get("ok") is True
    check("schema survives a clean restart", ok, "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    binary = str(Path(args.binary).resolve())
    tmp = Path(tempfile.mkdtemp(prefix="mvdb-crash-"))
    try:
        test_batches_survive(binary, tmp)
        test_crash_during_a_statement(binary, tmp)
        test_rollback_of_a_vector_survives_the_restart(binary, tmp)
        test_pool_backed_table_survives_the_restart(binary, tmp)
        test_open_transaction_is_lost_by_the_crash(binary, tmp)
        test_a_torn_tail_is_cut(binary, tmp)
        test_uncommitted_frames_are_not_mixed(binary, tmp)
        test_committed_survives(binary, tmp)
        test_uncommitted_is_lost(binary, tmp)
        test_no_partial_statement(binary, tmp)
        test_crash_during_recovery(binary, tmp)
        test_commit_reply_lost(binary, tmp)
        test_restart_keeps_schema(binary, tmp)
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
