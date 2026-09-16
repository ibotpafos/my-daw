#!/usr/bin/env python3
"""My DAW e2e coverage ledger.

The public product surface is engine/bridge/daw.h. A feature is only really
covered end-to-end when a scenario in tests/e2e/ drives it through that ABI.
This script cross-references the header against the scenario sources and
against the white-box tests/, so gaps are explicit instead of implied.

Usage:
    python3 scripts/e2e_coverage.py              # human report
    python3 scripts/e2e_coverage.py --strict     # exit 1 unless every ABI
                                                 # function is covered by an
                                                 # e2e scenario (or by a
                                                 # documented manual gate).
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "engine" / "bridge" / "daw.h"
E2E_DIR = ROOT / "tests" / "e2e"
TESTS_DIR = ROOT / "tests"

# A function counts as "driven" when called; the second pattern catches ABI
# names referenced as function pointers (e.g. the RAII deleter for daw_destroy).
CALL_RE = re.compile(r"\bdaw_[a-z0-9_]+(?=\s*\()")
REF_RE = re.compile(r"[,(\s]\s*(daw_[a-z0-9_]+)\s*[,)=]")

# Functions that cannot be exercised by a headless suite by policy (they would
# open a microphone/TCC prompt or need attached MIDI hardware or third-party
# vendor plug-ins). Each must name its manual gate so the gap is honest, not
# forgotten.
MANUAL_GATES = {
    "daw_record_start": "live input device + microphone permission; see hardware gates and docs/66",
    "daw_record_start_take": "live input device + microphone permission; see hardware gates and docs/66",
}

def declared_functions(text):
    decls = set()
    for match in re.finditer(r"^[a-zA-Z_][\w \*\t]*?\b(daw_[a-z0-9_]+)\s*\(", text, re.M):
        decls.add(match.group(1))
    return decls

def called_in(paths):
    calls = set()
    for path in paths:
        text = path.read_text(encoding="utf-8")
        calls.update(CALL_RE.findall(text))
        calls.update(REF_RE.findall(text))
    return calls

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    abi = declared_functions(HEADER.read_text(encoding="utf-8"))
    if not abi:
        print("error: found no daw_* declarations in", HEADER, file=sys.stderr)
        return 2

    e2e_sources = sorted(E2E_DIR.glob("e2e_*.cpp")) + sorted(E2E_DIR.glob("e2e.hpp"))
    e2e_calls = called_in(e2e_sources)
    whitebox_calls = called_in(sorted(TESTS_DIR.glob("*.cpp")))

    covered = sorted(abi & e2e_calls)
    gated = sorted(name for name in abi - e2e_calls if name in MANUAL_GATES)
    whitebox_only = sorted(
        name for name in abi - e2e_calls
        if name not in MANUAL_GATES and name in whitebox_calls
    )
    missing = sorted(
        name for name in abi - e2e_calls
        if name not in MANUAL_GATES and name not in whitebox_calls
    )

    print("public ABI functions:        " + str(len(abi)))
    print("covered by e2e scenarios:    " + str(len(covered)) + "  (" +
          str(len(covered) * 100 // max(1, len(abi))) + "%)")
    print("manual hardware gates:       " + str(len(gated)))
    print("white-box tests only:        " + str(len(whitebox_only)))
    print("not covered anywhere:        " + str(len(missing)))
    if gated:
        print("")
        print("manual gates (by policy, listed with reason):")
        for name in gated:
            print("  " + name + ": " + MANUAL_GATES[name])
    if whitebox_only:
        print("")
        print("reachable from white-box tests but not from an e2e scenario:")
        for name in whitebox_only:
            print("  " + name)
    if missing:
        print("")
        print("NOT COVERED ANYWHERE (must be fixed):")
        for name in missing:
            print("  " + name)
    if args.strict and (missing or whitebox_only):
        print("")
        print("strict mode: e2e coverage is incomplete", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
