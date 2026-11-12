#!/usr/bin/env python3
"""Local regression driver for MiniVectorDB.

Runs the mysqltest style cases (a .test file with SQL and driver directives
plus a .result file with the expected JSON assertions) against the database
binary over the JSON lines protocol.

    python3 tests/run_cases.py --binary ./minivectordb --package <dir>

The driver implements the same assertion rules as the contest driver:
  {}                 the statement must succeed
  {"error": name}    the statement must fail
  {"affected_rows": n}
  {"columns": [...], "rows": [[...]]}   with an optional "unordered": true
Floats are compared with an absolute and relative tolerance of 1e-5.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ABS_TOL = 1e-5
REL_TOL = 1e-5


def same(actual, expected) -> bool:
    if isinstance(expected, bool):
        return actual is expected
    if isinstance(expected, float):
        return (isinstance(actual, (int, float)) and not isinstance(actual, bool)
                and math.isfinite(actual)
                and math.isclose(actual, expected, abs_tol=ABS_TOL, rel_tol=REL_TOL))
    if isinstance(expected, list):
        return (isinstance(actual, list) and len(actual) == len(expected)
                and all(same(a, e) for a, e in zip(actual, expected)))
    if isinstance(expected, dict):
        return (isinstance(actual, dict) and set(actual) == set(expected)
                and all(same(actual[k], expected[k]) for k in expected))
    return type(actual) is type(expected) and actual == expected


class Database:
    """One database process, spoken to over stdin/stdout."""

    def __init__(self, binary: str, data_dir: Path):
        self.binary = binary
        self.data_dir = data_dir
        self.proc: subprocess.Popen | None = None
        self.seq = 0
        self.stderr_path = data_dir.parent / (data_dir.name + ".stderr.log")

    def start(self):
        data_dir = self.data_dir
        data_dir.mkdir(parents=True, exist_ok=True)
        err = open(self.stderr_path, "ab", buffering=0)
        self.proc = subprocess.Popen(
            [self.binary, "--data-dir", str(data_dir)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err,
            start_new_session=True)
        line = self._readline(30.0)
        if line is None:
            raise RuntimeError("no ready message")
        msg = json.loads(line)
        if msg != {"type": "ready"}:
            raise RuntimeError(f"unexpected handshake: {msg!r}")

    def _readline(self, timeout: float):
        assert self.proc and self.proc.stdout
        deadline = time.monotonic() + timeout
        buf = b""
        while b"\n" not in buf:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("timeout while reading a response")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("the database exited unexpectedly")
            buf += chunk
        line, rest = buf.split(b"\n", 1)
        self._pending = rest
        return line.decode("utf-8")

    def request(self, sql: str | None = None, mode: str = "exact", op: str = "execute"):
        if not hasattr(self, "trace"):
            self.trace = []
        if sql is not None:
            self.trace.append(sql)
        assert self.proc and self.proc.stdin
        self.seq += 1
        req = {"id": self.seq, "op": op}
        if sql is not None:
            req["sql"] = sql
            req["search_mode"] = mode
        self.proc.stdin.write((json.dumps(req, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        line = self._readline(60.0)
        return json.loads(line)

    def shutdown(self):
        if not self.proc:
            return
        try:
            resp = self.request(op="shutdown")
            if resp.get("ok") is not True:
                raise RuntimeError(f"shutdown failed: {resp!r}")
        except Exception:
            pass
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        rc = self.proc.wait(timeout=30)
        extra = self.proc.stdout.read()
        self.proc = None
        if rc != 0:
            raise RuntimeError(f"exit code {rc} after shutdown")
        if extra:
            raise RuntimeError(f"unexpected output after shutdown: {extra!r}")

    def kill(self):
        """Simulates a crash: SIGKILL without a clean shutdown."""
        if not self.proc:
            return
        os.killpg(self.proc.pid, 9)
        self.proc.wait(timeout=30)
        self.proc = None


def must(db: Database, sql: str, mode: str = "exact"):
    """Runs a statement that has to succeed.

    A setup statement that fails silently makes the rest of the test
    meaningless, so the tests use this instead of a bare request().
    """
    resp = db.request(sql, mode)
    if resp.get("ok") is not True:
        raise RuntimeError(f"the statement failed: {sql!r} -> {resp!r}")
    return resp


def check_case(binary: str, case: dict, package: Path, keep: bool = False) -> tuple[bool, str]:
    test_path = package / case["test"]
    result_path = package / case["result"]
    expected = json.loads(result_path.read_text(encoding="utf-8"))
    statements = [line.strip() for line in test_path.read_text(encoding="utf-8").splitlines()]
    tmp = Path(tempfile.mkdtemp(prefix="mvdb-case-"))
    data_dir = tmp / "data"
    mode = "exact"
    index = 0
    db = Database(binary, data_dir)
    try:
        db.start()
        for lineno, sql in enumerate(statements, 1):
            if not sql or sql.startswith("#"):
                continue
            if sql == "--restart":
                db.shutdown()
                db = Database(binary, data_dir)
                db.start()
                continue
            if sql in ("--mode exact", "--mode ann"):
                mode = sql.split()[1]
                continue
            if sql.startswith("--"):
                raise RuntimeError(f"unsupported directive: {sql}")
            if index >= len(expected):
                raise RuntimeError("more statements than expectations")
            resp = db.request(sql, mode)
            problem = compare(resp, expected[index])
            if problem:
                return False, f"line {lineno}: {sql}\n    {problem}\n    got      {json.dumps(resp, ensure_ascii=False)}\n    expected {json.dumps(expected[index], ensure_ascii=False)}"
            index += 1
        if index != len(expected):
            return False, f"{len(expected) - index} expectations were not reached"
        db.shutdown()
        return True, ""
    except Exception as exc:  # noqa: BLE001
        return False, f"error: {exc}"
    finally:
        try:
            if db.proc:
                db.kill()
        except Exception:
            pass
        if keep:
            print(f"    kept {tmp}", file=sys.stderr)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


def compare(resp: dict, want: dict) -> str | None:
    if "error" in want:
        if resp.get("ok") is not False:
            return "the statement should have failed"
        code = resp.get("error", {}).get("code")
        if not isinstance(code, str) or not code:
            return "the error has no code"
        return None
    if resp.get("ok") is not True:
        return f"the statement failed: {resp.get('error')}"
    for key in ("affected_rows", "columns"):
        if key in want and not same(resp.get(key), want[key]):
            return f"{key}: {resp.get(key)!r} != {want[key]!r}"
    if "rows" not in want:
        return None
    actual = resp.get("rows")
    if not isinstance(actual, list):
        return "the response has no rows"
    if want.get("unordered"):
        remaining = list(actual)
        for row in want["rows"]:
            hit = next((i for i, cand in enumerate(remaining) if same(cand, row)), None)
            if hit is None:
                return f"missing row {row!r}"
            remaining.pop(hit)
        if remaining:
            return f"unexpected rows {remaining!r}"
        return None
    if not same(actual, want["rows"]):
        return f"rows: {actual!r} != {want['rows']!r}"
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    ap.add_argument("--package", default="tests/package",
                    help="directory with manifest.json and cases/")
    ap.add_argument("--case", action="append", default=[])
    ap.add_argument("--keep", action="store_true", help="keep the temporary directories")
    args = ap.parse_args()

    package = Path(args.package)
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    cases = manifest["cases"]
    if args.case:
        wanted = set(args.case)
        cases = [c for c in cases if c["id"] in wanted]
    binary = str(Path(args.binary).resolve())

    failed = []
    t0 = time.monotonic()
    for case in cases:
        start = time.monotonic()
        ok, message = check_case(binary, case, package, args.keep)
        took = (time.monotonic() - start) * 1000
        print(f"{case['id']:<34} {'PASS' if ok else 'FAIL'}  {took:7.1f} ms", flush=True)
        if not ok:
            failed.append((case["id"], message))
    print(f"\n{len(cases) - len(failed)}/{len(cases)} passed in {time.monotonic() - t0:.1f} s")
    for name, message in failed:
        print(f"\n--- {name}\n{message}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
