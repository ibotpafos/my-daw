#!/usr/bin/env python3
"""Fail closed on missing, failed, skipped or incomplete CTest JUnit evidence."""
import argparse
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def verify_report(path: Path, required: list[str]) -> int:
    root = ET.parse(path).getroot()
    if root.tag not in {"testsuite", "testsuites"}:
        raise ValueError("Expected a JUnit testsuite or testsuites root")
    cases = list(root.iter("testcase"))
    if not cases:
        raise ValueError("JUnit contains no test cases")
    names = [case.get("name", "") for case in cases]
    if any(not name for name in names) or len(set(names)) != len(names):
        raise ValueError("JUnit has missing or duplicate CTest names")
    for suite in root.iter("testsuite"):
        for key in ("failures", "errors", "skipped", "disabled"):
            if int(suite.get(key, "0")) != 0:
                raise ValueError(f"JUnit suite reports {key}")
    for case in cases:
        if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")):
            raise ValueError(f"Test did not pass: {case.get('name')}")
        if case.get("status", "run") not in {"run", "passed"}:
            raise ValueError(f"Test was not run successfully: {case.get('name')}")
    missing = set(required) - set(names)
    if missing:
        raise ValueError(f"Required tests absent: {', '.join(sorted(missing))}")
    return len(cases)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--require", action="append", default=[])
    args = parser.parse_args()
    try:
        count = verify_report(args.report, args.require)
    except (OSError, ValueError, ET.ParseError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"PASS: {count} executed JUnit cases, no failures/skips, required tests present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
