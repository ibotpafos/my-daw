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

NL = chr(10)

def skip_path(path):
    """True for generated or hand-designed assets that are not scanned."""
    return path.name == "index.html" or "docs/assets" in str(path).replace(chr(92), "/")


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


def check_script_hygiene():
    """Reject leaked foreign scripts in readable text and sources.

    Two defect classes this repository has actually shipped: Cyrillic glued
    to Latin inside one word (invisible in review, unreadable on screen) and
    Han/kana/Hangul characters left by an IME mis-toggle. Fullwidth plus and
    the decorative wave dash are intentional UI glyphs, so only scripts with
    no business here are flagged. Generated output (docs/index.html,
    docs/assets) is skipped: it is rendered from checked Markdown or is
    hand-made design material.
    """
    cyrillic_latin = re.compile(r"[\u0400-\u04ff][A-Za-z]|[A-Za-z][\u0400-\u04ff]")
    ideographs = re.compile(r"[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af]")
    roots = [("README.md", (".md",)), ("docs", (".md",)), ("specs", (".md",)),
             ("apps", (".swift", ".md")), ("engine", (".hpp", ".cpp", ".h", ".c")),
             ("tests", (".hpp", ".cpp", ".h", ".c")), ("scripts", (".py", ".sh")),
             (".github", (".yml", ".yaml"))]
    offenders = []
    scanned = 0
    for root, suffixes in roots:
        base = ROOT / root
        candidates = [base] if base.is_file() else sorted(p for p in base.rglob("*") if p.is_file())
        for path in candidates:
            if path.suffix not in suffixes or skip_path(path):
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            scanned += 1
            for number, line in enumerate(text.splitlines(), 1):
                # A backslash escape (Swift/C/Python `\n`, `\t`, `\uXXXX`) puts a
                # Latin letter right against the following text; that is source
                # syntax, not a mixed-script word.
                probe = re.sub(r"\\[0Abfnrtuvx]", " ", line)
                rule = "cjk-ideograph" if ideographs.search(probe) else (
                    "cyrillic-latin-glue" if cyrillic_latin.search(probe) else None)
                if rule:
                    offenders.append(
                        f"{path.relative_to(ROOT)}:{number}: [{rule}] {line.strip()[:90]}")
    assert not offenders, "Leaked script in text:" + NL + NL.join(offenders[:20])
    print(f"PASS: script hygiene ({scanned} files, no cyrillic/latin glue, no cjk)")



def check_version_sync():
    """Версия живёт в одном месте (VERSION), остальное обязано ей следовать.

    build-macos.sh берёт VERSION и кладёт его в собираемый бандл вместе с
    короткой ревизией git. Гейд ловит обратное: когда исходный plist или баннер
    README уехали вперёд либо отстали, а по версии билда невозможно понять,
    какая сборка перед человеком.
    """
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version), f"VERSION must be X.Y.Z, got {version!r}"
    plist = (ROOT / "apps/macos/Info.plist").read_text(encoding="utf-8")
    match = re.search(r"<key>CFBundleShortVersionString</key><string>([^<]*)</string>", plist)
    assert match and match.group(1) == version, (
        f"apps/macos/Info.plist says {match and match.group(1)!r}, VERSION says {version!r}"
    )
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    assert f"прототип {version}" in readme, f"README banner is missing the current version {version}"
    roadmap = (ROOT / "docs/11-roadmap.md").read_text(encoding="utf-8")
    assert f"[{version}](" in roadmap, f"docs/11-roadmap.md has no entry for {version}"
    print(f"PASS: version {version} matches Info.plist, README banner and roadmap")

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
    check_script_hygiene()
    check_version_sync()
    check_sql()
    print("Documentation/contracts checked. This checker does not execute app, audio or recovery tests.")
