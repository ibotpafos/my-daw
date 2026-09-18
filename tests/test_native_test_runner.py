"""Portable runner regressions; no real user's crash reports are inspected."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from datetime import datetime, timezone, timedelta

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('native_test_runner', ROOT / 'scripts/native_test_runner.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class NativeTestRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.started = time.time() - 0.01
        self.report = self.root / 'export-tests-current.ips'
        self.executable = Path('/project/build/export-tests')

    def write_report(self, **overrides):
        report = {'pid': 123, 'procName': 'export-tests',
                  'procLaunch': datetime.now(timezone.utc).isoformat()}
        report.update(overrides)
        data = json.dumps({'bug_type': '309'}) + '\n' + json.dumps(report, indent=2)
        self.report.write_text(data)
        return data.encode()

    def test_two_object_report_matches_exact_child(self):
        data = self.write_report()
        self.assertEqual(runner.matched_report(self.report, 123, self.executable, self.started), data)

    def test_real_apple_launch_format_with_timezone_and_fractional_seconds(self):
        # Format observed in the actual macOS 15 IPS for our isolated child;
        # this space before the numeric offset is NOT accepted by fromisoformat.
        for offset in [0, -5, 5.5]:
            with self.subTest(offset=offset):
                date = datetime.now(timezone(timedelta(hours=offset)))
                launch = f'{date:%Y-%m-%d %H:%M:%S}.{date.microsecond // 100:04d} {date:%z}'
                data = self.write_report(procLaunch=launch)
                self.assertEqual(runner.matched_report(
                    self.report, 123, self.executable, self.started), data)

    def test_rejects_naive_or_future_launch_in_both_formats(self):
        future = datetime.now(timezone.utc) + timedelta(days=1)
        for launch in [datetime.now().isoformat(), future.isoformat(),
                       future.strftime('%Y-%m-%d %H:%M:%S.%f %z')]:
            with self.subTest(launch=launch):
                self.write_report(procLaunch=launch)
                self.assertIsNone(runner.matched_report(
                    self.report, 123, self.executable, self.started))

    def test_rejects_other_pid_name_or_earlier_launch(self):
        for changes in [{'pid': 124}, {'pid': '123'}, {'procName': 'another-app'},
                        {'procLaunch': '2000-01-01T00:00:00+00:00'}, {'procLaunch': 'bad'}]:
            with self.subTest(changes=changes):
                self.write_report(**changes)
                self.assertIsNone(runner.matched_report(self.report, 123, self.executable, self.started))

    def test_rejects_stale_non_crash_truncated_or_non_dictionary(self):
        data = self.write_report()
        os.utime(self.report, (0, 0))
        self.assertIsNone(runner.matched_report(self.report, 123, self.executable, self.started))
        for invalid in [data.replace(b'309', b'288'), b'{}\n{"pid":', b'[]\n{}', b'{}\n[]']:
            self.report.write_bytes(invalid)
            self.assertIsNone(runner.matched_report(self.report, 123, self.executable, self.started))

    def test_rejects_symlink_and_oversized_report(self):
        self.write_report()
        link = self.root / 'export-tests-link.ips'; link.symlink_to(self.report)
        self.assertIsNone(runner.matched_report(link, 123, self.executable, self.started))
        with self.report.open('wb') as stream:
            stream.truncate(runner.MAX_REPORT_BYTES + 1)
        self.assertIsNone(runner.matched_report(self.report, 123, self.executable, self.started))

    def test_copies_only_matched_report(self):
        data = self.write_report()
        (self.root / 'private-unrelated.ips').write_bytes(data)
        out = self.root / 'evidence'
        found = runner.collect_reports([self.root], out, 123, self.executable, self.started, 0)
        self.assertEqual(len(found), 1)
        self.assertEqual((out / found[0]).read_bytes(), data)
        self.assertEqual(len(list(out.iterdir())), 1)

    def execute(self, code, timeout=2):
        out = self.root / 'evidence'
        status = runner.run_test([sys.executable, '-c', code], out, timeout, [], 0)
        return status, json.loads((out / 'run.json').read_text())

    def test_success_and_nonzero_keep_original_result_without_retry(self):
        for expected in [0, 7, 139]:
            marker = self.root / f'attempt-{expected}'
            status, report = self.execute(f'from pathlib import Path; '
                f'p=Path({str(marker)!r}); p.write_text(p.read_text()+"x" if p.exists() else "x"); '
                f'raise SystemExit({expected})')
            self.assertEqual(status, expected)
            self.assertEqual(report['exit_code'], expected)
            self.assertEqual(report['returncode'], expected)
            self.assertEqual(report['attempts'], 1)
            self.assertEqual(marker.read_text(), 'x')
            self.assertGreater(report['pid'], 0)

    def test_signal_preserved_not_converted_to_success(self):
        status, report = self.execute('import os, signal; os.kill(os.getpid(), signal.SIGTERM)')
        self.assertEqual(status, 128 + signal.SIGTERM)
        self.assertEqual(report['signal'], signal.SIGTERM)
        self.assertFalse(report['crash_report_found'])

    def test_timeout_is_not_a_crash(self):
        status, report = self.execute('import time; time.sleep(30)', timeout=0.05)
        self.assertEqual(status, 124)
        self.assertTrue(report['timed_out'])
        self.assertEqual(report['outcome'], 'timeout')

    def test_launch_error_is_explicit(self):
        status = runner.run_test([str(self.root / 'does-not-exist')], self.root / 'evidence', 1, [], 0)
        self.assertEqual(status, 127)
        report = json.loads((self.root / 'evidence/run.json').read_text())
        self.assertEqual(report['outcome'], 'launch_error')
        self.assertNotIn('pid', report)


if __name__ == '__main__':
    unittest.main()
