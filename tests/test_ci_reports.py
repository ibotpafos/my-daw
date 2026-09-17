"""Regression tests for fail-closed CTest evidence using the standard library."""
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from check_test_report import verify_report


class TestReports(unittest.TestCase):
    def verify(self, xml, required=('host',)):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'ctest.xml'
            path.write_text(xml)
            return verify_report(path, list(required))

    def test_ctest_pass(self):
        self.assertEqual(self.verify('<testsuite failures="0"><testcase name="host" status="run"/></testsuite>'), 1)

    def test_nested_pass(self):
        self.assertEqual(self.verify('<testsuites><testsuite><testcase name="host" status="passed"/></testsuite></testsuites>'), 1)

    def test_failed_skipped_notrun(self):
        for child in ('failure', 'error', 'skipped'):
            with self.subTest(child=child), self.assertRaises(ValueError):
                self.verify(f'<testsuite><testcase name="host"><{child}/></testcase></testsuite>')
        for status in ('notrun', 'fail', 'disabled'):
            with self.subTest(status=status), self.assertRaises(ValueError):
                self.verify(f'<testsuite><testcase name="host" status="{status}"/></testsuite>')

    def test_required_case_missing(self):
        with self.assertRaises(ValueError):
            self.verify('<testsuite><testcase name="other"/></testsuite>')

    def test_empty_report(self):
        with self.assertRaises(ValueError):
            self.verify('<testsuite tests="32"/>')

    def test_suite_failure_counter(self):
        for key in ('failures', 'errors', 'skipped', 'disabled'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.verify(f'<testsuite {key}="1"><testcase name="host"/></testsuite>')

    def test_duplicate_case(self):
        with self.assertRaises(ValueError):
            self.verify('<testsuite><testcase name="host"/><testcase name="host"/></testsuite>')

    def test_malformed_and_missing(self):
        with self.assertRaises(ET.ParseError):
            self.verify('<testsuite')
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(OSError):
            verify_report(Path(directory) / 'absent.xml', ['host'])


if __name__ == '__main__':
    unittest.main()
