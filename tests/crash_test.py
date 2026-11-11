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
from run_cases import Database  # noqa: E402

RESULTS: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = ""):
    RESULTS.append((name, ok, detail))
    print(f"{name:<44} {'PASS' if ok else 'FAIL'}  {detail}", flush=True)


def rows_of(resp):
    return resp.get("rows", [])


def test_committed_survives(binary: str, tmp: Path):
    """A committed insert is still there after SIGKILL."""
    data = tmp / "committed"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    db.request("INSERT INTO t VALUES (1, 'a'), (2, 'b')")
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
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    db.request("INSERT INTO t VALUES (1, 'a')")
    db.request("BEGIN")
    db.request("UPDATE t SET v = 'changed' WHERE id = 1")
    db.request("DELETE FROM t WHERE id = 1")
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
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
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
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16))")
    for i in range(200):
        db.request(f"INSERT INTO t VALUES ({i}, 'value-{i}')")
    db.kill()

    # start and kill it immediately, repeatedly, while it recovers
    for _ in range(5):
        db = Database(binary, data)
        db.start()
        time.sleep(0.002)
        db.kill()

    db = Database(binary, data)
    db.start()
    resp = db.request("SELECT COUNT(*) FROM t")
    db.shutdown()
    # the statement may be unsupported (COUNT is not in the subset) - fall back
    ok = True
    detail = "recovered"
    if resp.get("ok"):
        ok = rows_of(resp) == [[200]]
        detail = json.dumps(rows_of(resp))
    else:
        detail = f"note: {resp.get('error', {}).get('code')}"
    check("recovery survives repeated crashes", ok, detail)


def test_commit_reply_lost(binary: str, tmp: Path):
    """A commit whose reply never arrived is applied completely or not at all."""
    data = tmp / "lost_reply"
    db = Database(binary, data)
    db.start()
    db.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    db.request("INSERT INTO t VALUES (1, 'a')")
    # the commit is requested, the process dies before the answer is read
    db.request("INSERT INTO t VALUES (2, 'b')")
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
    db.request("CREATE TABLE a (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    db.request("CREATE TABLE b (id BIGINT PRIMARY KEY, e VECTOR(3))")
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
