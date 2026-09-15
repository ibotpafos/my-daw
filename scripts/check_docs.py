#!/usr/bin/env python3
"""Validate documentation and draft contracts; never executes project commands."""
import argparse
import copy
import json
from pathlib import Path
import re
import sqlite3
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def read_json(path):
    def reject_constant(value):
        raise ValueError(f"Non-JSON constant: {value}")
    return json.loads(path.read_text(), parse_constant=reject_constant)


def check_links():
    files = [ROOT / "README.md", *sorted((ROOT / "docs").rglob("*.md")),
             *sorted((ROOT / "specs").rglob("*.md"))]
    count = 0
    for path in files:
        text = path.read_text()
        assert text.count("```") % 2 == 0, f"Unclosed code fence: {path}"
        for target in re.findall(r"\]\(([^\s)]+)\)", text):
            parsed = urlsplit(target)
            if parsed.scheme or not parsed.path:
                continue
            resolved = (path.parent / unquote(parsed.path)).resolve()
            assert resolved.is_relative_to(ROOT), f"Escaping link: {path}: {target}"
            assert resolved.exists(), f"Broken link: {path}: {target}"
            count += 1
        refs = set(re.findall(r"\[\^([^\]]+)\](?!:)", text))
        defs = set(re.findall(r"^\[\^([^\]]+)\]:", text, re.M))
        assert refs <= defs, f"Undefined footnote: {path}: {refs - defs}"
    print(f"PASS: {len(files)} Markdown files, {count} local links, footnotes and fences")


def check_schemas(required):
    pairs = [("command", "command"), ("module", "module"), ("project-manifest", "manifest")]
    data = {name: read_json(ROOT / "specs" / "examples" / f"{name}.json")
            for _, name in pairs}
    schemas = {name: read_json(ROOT / "specs" / f"{name}.schema.json")
               for name, _ in pairs}
    assert data["command"]["projectId"] == data["manifest"]["projectId"]
    action_ids = [a["id"] for a in data["module"]["actions"]]
    assert len(action_ids) == len(set(action_ids))
    try:
        from jsonschema import Draft202012Validator
    except ImportError:
        if required:
            raise RuntimeError("jsonschema missing; use the documented .venv command")
        print("SKIP: full JSON Schema validation (jsonschema unavailable); JSON parsing passed")
        return
    for schema_name, example_name in pairs:
        schema = schemas[schema_name]
        Draft202012Validator.check_schema(schema)
        Draft202012Validator(schema).validate(data[example_name])
    command_validator = Draft202012Validator(schemas["command"])
    negatives = []
    for key, value in [("apiVersion", 1), ("expectedRevision", -1), ("operations", [])]:
        invalid = copy.deepcopy(data["command"])
        invalid[key] = value
        negatives.append(invalid)
    for value in [25, -121, "louder"]:
        invalid = copy.deepcopy(data["command"])
        invalid["operations"][1]["gainDb"] = value
        negatives.append(invalid)
    invalid = copy.deepcopy(data["command"])
    invalid["operations"][0]["type"] = "shell.execute"
    negatives.append(invalid)
    invalid = copy.deepcopy(data["command"])
    invalid["operations"][0]["extra"] = True
    negatives.append(invalid)
    for invalid in negatives:
        assert not command_validator.is_valid(invalid), "Negative command accepted"
    module_invalid = copy.deepcopy(data["module"])
    module_invalid["capabilities"].append("network.unrestricted")
    assert not Draft202012Validator(schemas["module"]).is_valid(module_invalid)
    manifest_invalid = copy.deepcopy(data["manifest"])
    manifest_invalid["database"] = "../outside.sqlite"
    assert not Draft202012Validator(schemas["project-manifest"]).is_valid(manifest_invalid)
    print("PASS: 3 JSON Schemas, 3 examples, 10 negative schema cases")


def check_sql():
    db = sqlite3.connect(":memory:")
    db.executescript((ROOT / "specs/project-v0.sql").read_text())
    db.execute("INSERT INTO project VALUES (1, 'project-demo', 'Demo', 48000, 120, 0)")
    db.execute("INSERT INTO tracks(id, project_id, name, position, channels) VALUES (?, ?, ?, ?, ?)",
               ("track-vocal", "project-demo", "Vocal", 0, 1))
    db.execute("INSERT INTO assets VALUES (?, ?, ?, ?, ?, ?)",
               ("asset-1", "Media/example.wav", "a" * 64, 48000, 1, 96000))
    db.execute("INSERT INTO clips(id, track_id, asset_id, start_frame, source_offset, duration_frames) VALUES (?, ?, ?, ?, ?, ?)",
               ("clip-1", "track-vocal", "asset-1", 0, 0, 48000))
    db.commit()
    negatives = [
        ("UPDATE tracks SET gain_db = 25", ()),
        ("UPDATE tracks SET pan = 2", ()),
        ("UPDATE clips SET duration_frames = -1", ()),
        ("UPDATE clips SET fade_in_frames = 50000", ()),
        ("UPDATE clips SET asset_id = 'missing'", ()),
        ("UPDATE assets SET sha256 = ?", ("z" * 64,)),
        ("DELETE FROM assets WHERE id = 'asset-1'", ()),
        ("INSERT INTO project VALUES (2, 'second', 'Two', 48000, 120, 0)", ()),
    ]
    for query, params in negatives:
        try:
            db.execute(query, params)
        except sqlite3.IntegrityError:
            db.rollback()
        else:
            raise AssertionError(f"SQL constraint missing: {query}")
    # SQL engine rollback proof only, not an application command executor test.
    try:
        with db:
            db.execute("UPDATE tracks SET name = 'Changed'")
            db.execute("UPDATE tracks SET gain_db = 25")
    except sqlite3.IntegrityError:
        pass
    assert db.execute("SELECT name FROM tracks").fetchone()[0] == "Vocal"
    assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    assert not db.execute("PRAGMA foreign_key_check").fetchall()
    db.close()
    print("PASS: SQL v0, 8 negative constraints, transaction rollback, integrity")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--require-schemas", action="store_true")
    args = parser.parse_args()
    check_links()
    check_schemas(args.require_schemas)
    check_sql()
    print("Documentation/contracts checked. This checker does not execute app, audio or recovery tests.")
