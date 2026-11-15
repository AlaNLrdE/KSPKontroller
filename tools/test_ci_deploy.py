"""Exercise deploy_all orchestration in Bash with every hardware/build operation replaced by a stub."""
import os
import ntpath
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def find_bash():
    if os.name == 'nt':
        for variable in ('ProgramFiles', 'ProgramW6432', 'LOCALAPPDATA'):
            candidate = Path(os.environ.get(variable, '')) / 'Git/bin/bash.exe'
            if candidate.is_file():
                return str(candidate)
    return shutil.which('bash')


class DeployAllTest(unittest.TestCase):
    def test_reference_assembly_path_is_clean_with_empty_and_warm_cache(self):
        ci = (ROOT / 'ci').read_text(encoding='utf-8')
        helper = re.search(r'^ensure_refassemblies\(\) \{\n.*?^\}', ci, re.M | re.S)
        self.assertIsNotNone(helper)
        for windows in (0, 1):
            with self.subTest(windows=windows), tempfile.TemporaryDirectory(prefix='ksp refs ') as directory:
                script = '''set -euo pipefail
log() { printf '==> %s\\n' "$*"; }
is_windows() { [ "$FAKE_WINDOWS" = 1 ]; }
pwd() {
  if [ "${1:-}" = -W ]; then printf '%s\\n' 'C:/test refs/build/.NETFramework/v3.5';
  else command pwd; fi
}
curl() { printf 'fake archive' > "$3"; }
unzip() { mkdir -p "$REFASM_DIR/build/.NETFramework/v3.5"; touch "$REFASM_DIR/build/.NETFramework/v3.5/mscorlib.dll"; }
'''
                script += helper.group() + '\nrefs="$(ensure_refassemblies)"\nprintf "%s\\n" "$refs"\n'
                env = dict(os.environ, REFASM_DIR=Path(directory).as_posix(), REFASM_VERSION='test', FAKE_WINDOWS=str(windows))
                for cold in (True, False):
                    result = subprocess.run([find_bash(), '--noprofile', '--norc', '-c', script],
                                            env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(len(result.stdout.splitlines()), 1, result.stdout)
                    path = result.stdout.strip()
                    self.assertTrue(ntpath.isabs(path) if windows else os.path.isabs(path), path)
                    self.assertNotIn('==>', path)
                    self.assertEqual('Downloading .NET 3.5' in result.stderr, cold)

    def run_flow(self, **settings):
        bash = find_bash()
        self.assertIsNotNone(bash, 'Bash is required to validate the deployment command')
        ci = (ROOT / 'ci').read_text(encoding='utf-8')
        # Source only the two functions under test: never execute ci's dispatch
        # or load actual install/flash/SSH implementations.
        functions = []
        for name in ('deploy_all', 'hmi_preflight'):
            match = re.search(r'^' + name + r'\(\) \{\n.*?^\}', ci, re.M | re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        script = '''set -euo pipefail
record() { printf '%s\\n' "$1" >> "$TRACE"; }
log() { :; }
die() { printf '%s\\n' "$*" >&2; exit 1; }
find_kspdir() { printf '%s\\n' fake-ksp; }
ksp_running() { [ "${RUNNING:-0}" = 1 ]; }
find_board_port() { [ "${NO_BOARD:-0}" = 1 ] || printf '%s\\n' SIMPORT; }
hmi_ssh() {
  if [ "$1" = true ]; then
    record pi_reachable
    [ "${PI_OFFLINE:-0}" != 1 ]
    return
  fi
  record pi_packages
  if [ "${MISSING_PACKAGE:-0}" = 1 ]; then printf '%s\\n' 'cmake not installed'; fi
  return 0
}
build_KSPSerialIO() { record build_plugin; }
build_sketch() { record build_sketch; }
deploy_hmi() { record deploy_pi; [ "${PI_BUILD_FAIL:-0}" != 1 ]; }
install_plugin() { record install_plugin; }
upload_sketch() { record flash_board; printf '%s\\n' "$1" > "$BUILD/last_upload_port"; }
run_board_test() { record test_board; }
sleep() { :; }
'''
        script += '\n\n'.join(functions) + '\ndeploy_all\n'
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            trace = folder / 'trace.txt'
            env = dict(os.environ, BUILD=folder.as_posix(), TRACE=trace.as_posix(),
                       HMI_HOST='fake-pi', HMI_APT_PACKAGES='cmake', FQBN='fake-board',
                       ARDUINO_PORT='', NO_TEST='0')
            env.update({k: str(v) for k, v in settings.items()})
            result = subprocess.run([bash, '--noprofile', '--norc', '-c', script], env=env,
                                    capture_output=True, text=True, timeout=10)
            calls = trace.read_text().splitlines() if trace.exists() else []
        return result, calls

    def test_all_three_projects_and_pi_before_hardware_writes(self):
        result, calls = self.run_flow()
        self.assertEqual(result.returncode, 0, result.stderr)
        for call in ('build_plugin', 'build_sketch', 'deploy_pi', 'install_plugin', 'flash_board', 'test_board'):
            self.assertEqual(calls.count(call), 1, calls)
        self.assertLess(calls.index('pi_packages'), calls.index('build_plugin'))
        self.assertLess(calls.index('build_plugin'), calls.index('deploy_pi'))
        self.assertLess(calls.index('build_sketch'), calls.index('deploy_pi'))
        self.assertLess(calls.index('deploy_pi'), calls.index('install_plugin'))
        self.assertLess(calls.index('deploy_pi'), calls.index('flash_board'))

    def test_unreachable_pi_stops_before_build_install_or_flash(self):
        result, calls = self.run_flow(PI_OFFLINE=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, ['pi_reachable'])

    def test_missing_pi_dependencies_stop_before_build_install_or_flash(self):
        result, calls = self.run_flow(MISSING_PACKAGE=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, ['pi_reachable', 'pi_packages'])

    def test_failed_pi_build_cannot_install_plugin_or_flash_board(self):
        result, calls = self.run_flow(PI_BUILD_FAIL=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('deploy_pi', calls)
        self.assertNotIn('install_plugin', calls)
        self.assertNotIn('flash_board', calls)

    def test_no_test_only_skips_board_test(self):
        result, calls = self.run_flow(NO_TEST=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('deploy_pi', calls)
        self.assertIn('flash_board', calls)
        self.assertNotIn('test_board', calls)

    def test_game_running_or_board_missing_prevents_deployment(self):
        for settings in ({'RUNNING': 1}, {'NO_BOARD': 1}):
            with self.subTest(settings=settings):
                result, calls = self.run_flow(**settings)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
