#!/usr/bin/env python3
"""The acceptance check of the protocol specification.

STDIO_PROTOCOL.md, section 7 describes a batch request file and the
assertions that the answer must satisfy byte for byte.  This script runs the
same requests against the database and applies the same assertions, so the
protocol can be verified without docker.

    python3 tests/spec_check.py --binary ./minivectordb
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REQUESTS = [
    {"id": 1, "op": "execute", "sql": "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));",
     "search_mode": "exact"},
    {"id": 2, "op": "execute", "sql": "INSERT INTO docs VALUES (1, '你好');", "search_mode": "exact"},
    {"id": 3, "op": "execute", "sql": "SELECT id, content FROM docs ORDER BY id;", "search_mode": "exact"},
    {"id": 4, "op": "shutdown"},
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default="./minivectordb")
    args = ap.parse_args()
    binary = str(Path(args.binary).resolve())

    tmp = Path(tempfile.mkdtemp(prefix="mvdb-spec-"))
    try:
        data = tmp / "data"
        data.mkdir(parents=True, exist_ok=True)
        payload = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in REQUESTS)
        proc = subprocess.run([binary, "--data-dir", str(data)], input=payload.encode("utf-8"),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        raw = proc.stdout
        (tmp / "stdout.jsonl").write_bytes(raw)
        (tmp / "stderr.log").write_bytes(proc.stderr)

        problems = []
        if proc.returncode != 0:
            problems.append(f"the exit code is {proc.returncode}, expected 0")
        if not raw.endswith(b"\n"):
            problems.append("the last response must end with a newline")
        lines = raw.decode("utf-8").splitlines()
        if len(lines) != 5:
            problems.append(f"expected 5 lines, got {len(lines)}: {lines[:6]!r}")
        else:
            messages = [json.loads(line) for line in lines]
            if messages[0] != {"type": "ready"}:
                problems.append(f"the handshake is wrong: {messages[0]!r}")
            for ident, response in enumerate(messages[1:], 1):
                if type(response.get("id")) is not int or response["id"] != ident:
                    problems.append(f"the id of response {ident} is wrong: {response!r}")
                if response.get("ok") is not True:
                    problems.append(f"response {ident} is not ok: {response!r}")
            if type(messages[2].get("affected_rows")) is not int:
                problems.append("affected_rows is not an integer")
            elif messages[2]["affected_rows"] != 1:
                problems.append(f"affected_rows is {messages[2]['affected_rows']}, expected 1")
            if messages[3].get("columns") != ["id", "content"]:
                problems.append(f"the columns are wrong: {messages[3].get('columns')!r}")
            if messages[3].get("rows") != [[1, "你好"]]:
                problems.append(f"the rows are wrong: {messages[3].get('rows')!r}")
        for problem in problems:
            print("FAIL:", problem, file=sys.stderr)
        if problems:
            return 1
        print("基础协议样例通过 (the protocol sample of STDIO_PROTOCOL.md passes)")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
