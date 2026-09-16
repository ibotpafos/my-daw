"""Repository invariants tested with the Python standard library, not a custom linter."""
import json
from pathlib import Path
import subprocess
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProjectLayoutTests(unittest.TestCase):
    def test_swift_manifest_is_complete_and_unique(self):
        entries = [line.strip() for line in (ROOT / 'apps/macos/sources.txt').read_text().splitlines()
                   if line.strip() and not line.lstrip().startswith('#')]
        self.assertEqual(len(entries), len(set(entries)), 'Duplicate compiler inputs')
        actual = {path.relative_to(ROOT).as_posix() for path in (ROOT / 'apps/macos').rglob('*.swift')}
        self.assertEqual(set(entries), actual, 'Update sources.txt when moving/adding app sources')
        self.assertIn('apps/macos/main.swift', entries)
        self.assertIn('apps/macos/ProjectTime.swift', entries)
        for entry in entries:
            self.assertFalse(Path(entry).is_absolute())
            self.assertNotIn('..', Path(entry).parts)

    def test_presets_fail_when_no_tests_are_discovered(self):
        presets = json.loads((ROOT / 'CMakePresets.json').read_text())
        for preset in presets['testPresets']:
            with self.subTest(preset=preset['name']):
                self.assertEqual(preset['execution']['noTestsAction'], 'error')
                self.assertIn('outputJUnitFile', preset['output'])
        debug = next(p for p in presets['configurePresets'] if p['name'] == 'debug')
        self.assertEqual(debug['cacheVariables']['DAW_SANITIZERS'], 'OFF')
        self.assertEqual(debug['cacheVariables']['DAW_THREAD_SANITIZER'], 'OFF')

    def test_cmake_uses_the_product_version(self):
        source = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('/VERSION', source)
        self.assertIn('project(MyDAW VERSION ${DAW_VERSION}', source)
        self.assertIn('include(cmake/DawPlatform.cmake)', source)
        self.assertIn('include(cmake/DawTests.cmake)', source)

    def test_codex_and_legacy_alias_share_the_entrypoint(self):
        environment = tomllib.loads((ROOT / '.codex/environments/environment.toml').read_text())
        run = next(action for action in environment['actions'] if action['name'] == 'Run')
        self.assertEqual(run['command'], './script/build_and_run.sh')
        alias = (ROOT / 'scripts/run-macos.sh').read_text()
        self.assertIn('exec "$ROOT_DIR/script/build_and_run.sh" "$@"', alias)
        for name in ['script/build_and_run.sh', 'scripts/run-macos.sh']:
            self.assertNotIn('pkill -', (ROOT / name).read_text())

    def test_documented_runners_are_executable_in_git(self):
        names = ['script/build_and_run.sh', 'scripts/run-macos.sh', 'scripts/test-midi-hardware.sh',
                 'scripts/test-au-music-device.sh', 'scripts/test-audio-preview.sh']
        output = subprocess.check_output(['git', 'ls-files', '--stage', '--', *names], cwd=ROOT, text=True)
        modes = {line.split('\t', 1)[1]: line.split()[0] for line in output.splitlines()}
        for name in names:
            self.assertEqual(modes.get(name), '100755', name)

    def test_catalog_types_do_not_import_platform_runtime(self):
        catalog = (ROOT / 'engine/plugins/vst3_catalog.hpp').read_text()
        self.assertIn('struct Vst3ScannedClass', catalog)
        self.assertIn('struct Vst3ScanCacheEntry', catalog)
        self.assertNotIn('#include "platform/', catalog)
        self.assertNotIn('#include <AudioToolbox', catalog)


if __name__ == '__main__':
    unittest.main()
