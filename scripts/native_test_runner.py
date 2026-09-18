#!/usr/bin/env python3
"""Run a native test ONCE, preserving its exit status and PID-matched diagnostics.

Crash reports use Apple's two-JSON-object IPS format. Never upload the whole
DiagnosticReports directory: only recent crash reports for this exact child.
This runner changes no application behavior and does not retry failed tests.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

MAX_REPORT_BYTES = 8 * 1024 * 1024
MAX_CANDIDATES = 32


def launch_time(value: str) -> float:
    """Accept Apple's IPS date (including its space before offset) and ISO dates."""
    try:
        parsed = datetime.strptime(value, '%Y-%m-%d %H:%M:%S.%f %z')
    except ValueError:
        parsed = datetime.fromisoformat(value)
    if parsed.tzinfo is None:
        raise ValueError('Crash launch time must carry its timezone')
    return parsed.timestamp()


def matched_report(path: Path, pid: int, executable: Path, started: float) -> bytes | None:
    """Reject unrelated, stale, incomplete, symlinked or oversized reports."""
    try:
        if path.is_symlink() or not path.is_file():
            return None
        stat = path.stat()
        if stat.st_mtime < started or stat.st_size > MAX_REPORT_BYTES:
            return None
        with path.open('rb') as stream:
            data = stream.read(MAX_REPORT_BYTES + 1)
        if len(data) > MAX_REPORT_BYTES:
            return None
        first, payload = data.split(b'\n', 1)
        header, report = json.loads(first), json.loads(payload)
        if not isinstance(header, dict) or not isinstance(report, dict):
            return None
        if str(header.get('bug_type')) != '309':
            return None
        if type(report.get('pid')) is not int or report['pid'] != pid:
            return None
        if report.get('procName') != executable.name:
            return None
        # procLaunch has subsecond precision; allow the scheduler/formatting
        # rounding at the beginning of the launch, never an earlier process.
        launched = launch_time(report['procLaunch'])
        if not math.isfinite(launched) or not started - 1 <= launched <= time.time() + 1:
            return None
        return data
    except (OSError, ValueError, KeyError, TypeError):
        return None


def collect_reports(directories: list[Path], destination: Path, pid: int,
                    executable: Path, started: float, wait_seconds: float = 10) -> list[str]:
    deadline = time.monotonic() + wait_seconds
    while True:
        candidates: list[Path] = []
        for directory in directories:
            try:
                # Do not read other applications' reports. macOS uses the
                # executable basename as the report filename prefix.
                candidates.extend(directory.glob(executable.name + '*.ips'))
            except OSError:
                continue
        def modified(path: Path) -> float:
            try:
                return path.stat().st_mtime
            except OSError:
                return 0
        candidates.sort(key=modified, reverse=True)
        found: list[str] = []
        seen: set[str] = set()
        for candidate in candidates[:MAX_CANDIDATES]:
            data = matched_report(candidate, pid, executable, started)
            if data is None:
                continue
            digest = hashlib.sha256(data).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            destination.mkdir(parents=True, exist_ok=True)
            name = f'crash-{pid}-{digest[:12]}.ips'
            (destination / name).write_bytes(data)
            found.append(name)
        if found or time.monotonic() >= deadline:
            return found
        time.sleep(min(0.25, max(0, deadline - time.monotonic())))


def run_test(command: list[str], evidence: Path, timeout: float,
             report_directories: list[Path] | None = None,
             report_wait: float = 10) -> int:
    evidence.mkdir(parents=True, exist_ok=True)
    executable = Path(command[0]).resolve()
    metadata = {'executable': str(executable), 'command': command, 'attempts': 1,
                'started_at': datetime.now(timezone.utc).isoformat(),
                'timeout_seconds': timeout, 'crash_reports': []}
    metadata_path = evidence / 'run.json'
    def write_metadata() -> None:
        metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + '\n')
    started = time.time()
    try:
        child = subprocess.Popen(command, start_new_session=True)
    except OSError as error:
        metadata.update(outcome='launch_error', error=str(error), exit_code=127)
        write_metadata()
        return 127
    metadata['pid'] = child.pid
    write_metadata()  # Survives a runner interruption; never a success marker.
    timed_out = False
    try:
        child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        if sys.platform == 'darwin':
            try:
                subprocess.run(['/usr/bin/sample', str(child.pid), '1', '1', '-file',
                                str(evidence / 'timeout-sample.txt')],
                               timeout=5, capture_output=True, check=False)
            except (OSError, subprocess.TimeoutExpired):
                pass
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
    code = 124 if timed_out else child.returncode if child.returncode >= 0 else 128 - child.returncode
    metadata.update(returncode=child.returncode, exit_code=code, timed_out=timed_out,
                    outcome='timeout' if timed_out else 'success' if code == 0 else 'failure',
                    finished_at=datetime.now(timezone.utc).isoformat())
    if child.returncode < 0:
        metadata['signal'] = -child.returncode
    write_metadata()
    if code != 0:
        directories = report_directories if report_directories is not None else ([
            Path.home() / 'Library/Logs/DiagnosticReports',
            Path('/Library/Logs/DiagnosticReports')
        ] if sys.platform == 'darwin' else [])
        if directories:
            report_dir = evidence / f'process-{child.pid}-{int(started * 1000000)}'
            reports = collect_reports(directories, report_dir, child.pid, executable, started, report_wait)
            metadata['crash_reports'] = [str(Path(report_dir.name) / name) for name in reports]
        metadata['crash_report_found'] = bool(metadata['crash_reports'])
        write_metadata()
        print(f'Native test failed: PID {child.pid}, exit {code}, '
              f'reports {len(metadata["crash_reports"])}. No retry performed.', file=sys.stderr)
    return code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('An executable and a positive finite timeout are required')
    return run_test(command, args.evidence, args.timeout)


if __name__ == '__main__':
    sys.exit(main())
