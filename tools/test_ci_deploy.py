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
    def test_wdt_metadata_override_preserves_sources_and_compile_options(self):
        ci = (ROOT / 'ci').read_text(encoding='utf-8')
        helper = re.search(r'^compile_sketch\(\) \{\n.*?^\}', ci, re.M | re.S)
        self.assertIsNotNone(helper)
        for architectures in ('renesas', 'renesas_uno', 'renesas,renesas_uno'):
            with self.subTest(architectures=architectures), tempfile.TemporaryDirectory(prefix='wdt test ') as directory:
                root = Path(directory)
                library = root / 'platform/1.6.0/libraries/WDT'
                (library / 'src').mkdir(parents=True)
                metadata = 'name=WDT\r\narchitectures=' + architectures + '\r\n'
                (library / 'library.properties').write_bytes(metadata.encode())
                (library / 'src/WDT.cpp').write_bytes(b'original watchdog implementation\n')
                script = '''set -euo pipefail
is_windows() { return 1; }
die() { printf '%s\\n' "$*" >&2; exit 1; }
fake_cli() {
  if [ "${2:-}" = --show-properties=unexpanded ]; then
    printf 'runtime.platform.path=%s\\n' "$PLATFORM"
  else printf '%s\\n' "$@" > "$TRACE"; fi
}
'''
                script += helper.group() + '\ncompile_sketch fake_cli --optimize-for-debug --build-path "build path"\n'
                env = dict(os.environ, FQBN='arduino:renesas_uno:unor4wifi', SKETCH='sketch path',
                           BUILD=(root / 'build').as_posix(), PLATFORM=(root / 'platform/1.6.0').as_posix(),
                           TRACE=(root / 'trace').as_posix())
                result = subprocess.run([find_bash(), '--noprofile', '--norc', '-c', script],
                                        env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                args = (root / 'trace').read_text().splitlines()
                self.assertEqual(args[-4:], ['--optimize-for-debug', '--build-path', 'build path', 'sketch path'])
                self.assertEqual('--library' in args, architectures == 'renesas')
                self.assertEqual((library / 'library.properties').read_bytes(), metadata.encode())
                if architectures == 'renesas':
                    override = root / 'build/wdt-compat/1.6.0/WDT'
                    self.assertIn('architectures=renesas,renesas_uno', (override / 'library.properties').read_text())
                    self.assertEqual((override / 'src/WDT.cpp').read_bytes(), (library / 'src/WDT.cpp').read_bytes())

    def test_pi_krpc_endpoint_prefers_override_then_kept_name_then_deployer_name(self):
        """deploy_hmi's endpoint for ~/dev/run/ksphmi/env: names survive IP changes and deploys from the Mac (user)."""
        ci = (ROOT / 'ci').read_text(encoding='utf-8')
        helper = re.search(r'^hmi_pc_address\(\) \{\n.*?^\}', ci, re.M | re.S)
        self.assertIsNotNone(helper)
        script = '''set -euo pipefail
die() { printf 'DIE %s\\n' "$*"; exit 3; }
hostname() { printf '%s\\n' "$DEPLOYER"; }
hmi_ssh_client() { echo fake_ssh; }
fake_ssh() { printf '%s\\n' "$SSH_FROM"; }
hmi_ssh() {
  case "$1" in
    *KSPHMI_KRPC_ADDRESS*) printf '%s' "$CURRENT" ;;
    *getent*) [ "$RESOLVES" = 1 ] ;;
  esac
}
''' + helper.group() + '\nhmi_pc_address\n'
        cases = [  # override, current env, deployer, ssh source (-4), Pi resolves deployer.local -> expected
            ('192.168.9.9', 'Alan-TR.local', 'Mac', '192.168.0.138', '1', '192.168.9.9'),
            ('', 'Alan-TR.local', 'Alans-MacBook-Air.local', '192.168.0.138', '1', 'Alan-TR.local'),
            ('', 'Alan-TR.local', 'Alan-TR', '192.168.0.152', '0', 'Alan-TR.local'),
            ('', '192.168.0.152', 'Alan-TR', '192.168.0.152', '1', 'Alan-TR.local'),
            ('', '', 'Alan-TR', '192.168.0.152', '1', 'Alan-TR.local'),
            ('', 'fe80::1%eth0', 'Alan-TR', '192.168.0.152', '0', '192.168.0.152'),
            ('', '', 'Alan-TR', '192.168.0.152', '0', '192.168.0.152'),
        ]
        for override, current, deployer, source, resolves, expected in cases:
            with self.subTest(current=current, deployer=deployer, resolves=resolves):
                env = dict(os.environ, HMI_KRPC_ADDRESS=override, CURRENT=current, DEPLOYER=deployer,
                           SSH_FROM=source, RESOLVES=resolves, HMI_HOST='ksphmi')
                result = subprocess.run([find_bash(), '--noprofile', '--norc', '-c', script],
                                        env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), expected)
        env = dict(os.environ, HMI_KRPC_ADDRESS='', CURRENT='', DEPLOYER='Alan-TR', SSH_FROM='fe80::2%eth0',
                   RESOLVES='0', HMI_HOST='ksphmi')
        result = subprocess.run([find_bash(), '--noprofile', '--norc', '-c', script],
                                env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 3, result.stdout)  # never an IPv6 endpoint for the IPv4-only HMI

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

    def test_hmi_reference_assembly_path_is_clean_with_empty_and_warm_cache(self):
        ci = (ROOT / 'ci').read_text(encoding='utf-8')
        helper = re.search(r'^ensure_hmi_refassemblies\(\) \{\n.*?^\}', ci, re.M | re.S)
        self.assertIsNotNone(helper)
        with tempfile.TemporaryDirectory(prefix='ksp refs ') as directory:
            script = '''set -euo pipefail
log() { printf '==> %s\\n' "$*"; }
is_windows() { return 1; }
curl() { printf 'fake archive' > "$3"; }
unzip() { mkdir -p "$HMI_REFASM_DIR/build/.NETFramework/v4.7.1"; touch "$HMI_REFASM_DIR/build/.NETFramework/v4.7.1/mscorlib.dll"; }
'''
            script += helper.group() + '\nrefs="$(ensure_hmi_refassemblies)"\nprintf "%s\\n" "$refs"\n'
            env = dict(os.environ, HMI_REFASM_DIR=Path(directory).as_posix(), REFASM_VERSION='test')
            for cold in (True, False):
                result = subprocess.run([find_bash(), '--noprofile', '--norc', '-c', script],
                                        env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(result.stdout.splitlines()), 1, result.stdout)
                self.assertTrue(result.stdout.strip().endswith('v4.7.1'), result.stdout)
                self.assertEqual('Downloading .NET 4.7.1' in result.stderr, cold)

    def test_hmi_plugin_builds_only_with_krpc_and_against_the_install(self):
        ci = (ROOT / 'ci').read_text(encoding='utf-8')
        helper = re.search(r'^build_KSPSerialIO_Hmi\(\) \{\n.*?^\}', ci, re.M | re.S)
        self.assertIsNotNone(helper)
        # The plugin build always runs it; KSPSerialIO.dll itself never references kRPC.
        build = re.search(r'^build_KSPSerialIO\(\) \{\n.*?^\}', ci, re.M | re.S)
        self.assertIn('build_KSPSerialIO_Hmi', build.group())
        with tempfile.TemporaryDirectory(prefix='ksp install ') as directory:
            ksp = Path(directory)
            script = '''set -euo pipefail
log() { printf '==> %s\\n' "$*"; }
find_kspdir() { [ -n "$FAKE_KSP" ] && printf '%s\\n' "$FAKE_KSP"; }
ensure_hmi_refassemblies() { printf '%s\\n' 'REFS 4.7.1'; }
msbuild() { printf '%s\\n' "$@" > "$TRACE"; }
xbuild() { msbuild "$@"; }
dotnet() { msbuild "$@"; }
'''
            script += helper.group() + '\nbuild_KSPSerialIO_Hmi\n'
            trace = ksp / 'trace.txt'
            base = dict(os.environ, TRACE=trace.as_posix(), CONFIG='Release', HMI_PLUGIN_PROJ='proj.csproj',
                        HMI_PLUGIN_OUT='out')
            for name, fake_ksp, krpc, built in (('no install', '', False, False), ('no kRPC', ksp.as_posix(), False, False),
                                                 ('kRPC installed', ksp.as_posix(), True, True)):
                with self.subTest(name):
                    if krpc:
                        (ksp / 'GameData/kRPC').mkdir(parents=True, exist_ok=True)
                        (ksp / 'GameData/kRPC/KRPC.Core.dll').write_bytes(b'')
                    result = subprocess.run([find_bash(), '--noprofile', '--norc', '-c', script],
                                            env=dict(base, FAKE_KSP=fake_ksp), capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(trace.exists(), built, result.stderr)
                    if built:
                        args = trace.read_text().splitlines()
                        self.assertIn('-p:KSPDIR=' + ksp.as_posix(), args)
                        self.assertIn('-p:FrameworkPathOverride=REFS 4.7.1', args)
                    else:
                        self.assertIn('KSPSerialIO.Hmi.dll not built', result.stderr)

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


def ci_functions(*names):
    ci = (ROOT / 'ci').read_text(encoding='utf-8')
    found = []
    for name in names:
        match = re.search(r'^' + name + r'\(\) \{\n.*?^\}', ci, re.M | re.S)
        assert match, name
        found.append(match.group())
    return '\n\n'.join(found) + '\n'


def run_bash(script, env, timeout=30):
    return subprocess.run([find_bash(), '--noprofile', '--norc', '-c', script], env=env,
                          capture_output=True, text=True, timeout=timeout)


class CrossDeployTest(unittest.TestCase):
    """deploy_hmi ships cross-built programs (user, 2026-11-17: never build on the Pi), with stubs for SSH and builds."""

    STUBS = '''set -euo pipefail
record() { printf '%s\\n' "$1" >> "$TRACE"; }
log() { printf '==> %s\\n' "$*"; }
die() { printf 'DIE %s\\n' "$*"; exit 1; }
is_windows() { return 1; }
'''

    def test_cross_path_when_toolchain_present_else_on_pi_build(self):
        stubs = self.STUBS + '''hmi_preflight() { record preflight; }
hmi_deploy_lock() { record lock; }
hmi_deploy_unlock() { record unlock; }
hmi_pc_address() { echo 192.0.2.1; }
hmi_cross_available() { [ "$CROSS" = 1 ]; }
build_hmi_pi() { record build_hmi_pi; }
hmi_ship_cross() { record ship_cross; }
hmi_copy_sources() { record copy_sources; }
hmi_build_on_pi() { record build_on_pi; }
hmi_install_service() { record "install $1"; }
hmi_kiosk() { record kiosk; }
'''
        script = stubs + ci_functions('deploy_hmi') + 'deploy_hmi\n'
        for cross, expected, warned in (
                ('1', ['preflight', 'lock', 'build_hmi_pi', 'ship_cross', 'install 192.0.2.1', 'kiosk', 'unlock'], False),
                ('0', ['preflight', 'lock', 'copy_sources', 'build_on_pi', 'install 192.0.2.1', 'kiosk', 'unlock'], True)):
            with self.subTest(cross=cross), tempfile.TemporaryDirectory() as directory:
                trace = Path(directory) / 'trace'
                result = run_bash(script, dict(os.environ, TRACE=trace.as_posix(), CROSS=cross, HMI_HOST='fake-pi',
                                               HMI_CROSS_APT='g++-aarch64-linux-gnu'))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(trace.read_text().splitlines(), expected)
                self.assertEqual('WARNING: no aarch64 cross compiler' in result.stdout, warned, result.stdout)

    def test_install_hmi_builds_natively_and_runs_the_pi_steps_locally(self):
        """./ci install_hmi on the Pi (user, 2026-11-19): a native build, then deploy_hmi's steps with HMI_HOST=local."""
        stubs = self.STUBS + '''uname() { case "$1" in -s) echo "$FAKE_OS" ;; -m) echo "$FAKE_ARCH" ;; esac; }
hmi_preflight() { record "preflight $HMI_HOST"; }
hmi_deploy_lock() { record lock; }
hmi_deploy_unlock() { record unlock; }
hmi_pc_address() { echo gamepc.local; }
build_hmi_pi_native() { record native; }
build_hmi_pi() { record build_hmi_pi; }
hmi_ship_cross() { record ship; }
hmi_install_service() { record "install $1"; }
hmi_kiosk() { record kiosk; }
'''
        script = stubs + ci_functions('install_hmi') + 'install_hmi\n'
        for os_name, arch, expected in (
                ('Linux', 'aarch64', ['preflight local', 'lock', 'native', 'ship', 'install gamepc.local', 'kiosk', 'unlock']),
                ('Linux', 'x86_64', []), ('Darwin', 'arm64', [])):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as directory:
                trace = Path(directory) / 'trace'
                trace.write_text('')
                result = run_bash(script, dict(os.environ, TRACE=trace.as_posix(), FAKE_OS=os_name, FAKE_ARCH=arch,
                                               HMI_HOST='ksphmi'))
                self.assertEqual(result.returncode == 0, bool(expected), result.stdout + result.stderr)
                self.assertEqual(trace.read_text().splitlines(), expected)

    def test_local_host_runs_the_pi_commands_here(self):
        script = self.STUBS + ci_functions('hmi_ssh') + 'cd /; HMI_HOST=local hmi_ssh "pwd; echo \\$0" </dev/null\n'
        with tempfile.TemporaryDirectory() as directory:
            result = run_bash(script, dict(os.environ, HOME=directory, TRACE=(Path(directory) / 't').as_posix()))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # resolve both: on macOS the temp dir is /var/... while its real path is /private/var/...
            self.assertEqual(str(Path(result.stdout.split()[0]).resolve()), str(Path(directory).resolve()))

    def ship(self, failing_test=''):
        """Run the real hmi_ship_cross against a fake Pi home (hmi_ssh = a local shell there)."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        home, hmi, build, bin_dir = root / 'pi', root / 'hmi-cpp', root / 'hmi-cpp-pi', root / 'bin'
        for name in ('CMakeLists.txt', 'config.toml'):
            (hmi / name).parent.mkdir(parents=True, exist_ok=True)
            (hmi / name).write_text('new ' + name, newline='\n')
        for folder in ('cmake', 'include', 'src', 'tests', 'systemd', 'assets'):
            (hmi / folder).mkdir(parents=True, exist_ok=True)
            (hmi / folder / 'file.txt').write_text('new', newline='\n')
        build.mkdir()
        tests = ['ksphmi_tests', 'ksphmi_render_tests']
        for name in ['ksphmi'] + tests:
            code = 1 if name == failing_test else 0
            (build / name).write_text('#!/bin/sh\necho ran %s >> "$HOME/ran"\nexit %d\n' % (name, code), newline='\n')
        bin_dir.mkdir()
        (bin_dir / 'python3').write_text('#!/bin/sh\necho "python3 $*" >> "$HOME/ran"\n', newline='\n')
        (bin_dir / 'python3').chmod(0o755)  # macOS/Linux skip a non-executable file on PATH (Git Bash does not)
        (home / '.local/share/ksphmi').mkdir(parents=True)
        (home / '.local/bin').mkdir(parents=True)
        (home / '.local/share/ksphmi/config.toml').write_text('old config', newline='\n')
        (home / '.local/bin/ksphmi').write_text('old binary', newline='\n')
        stubs = self.STUBS + '''hmi_ssh() { HOME="$FAKE_HOME" PATH="$(cd "$FAKE_BIN" && pwd):$PATH" bash -c "$1"; }
'''
        script = stubs + ci_functions('hmi_pack_sources', 'hmi_ship_cross', 'hmi_test_and_install') + 'hmi_ship_cross\n'
        env = dict(os.environ, FAKE_HOME=home.as_posix(), FAKE_BIN=bin_dir.as_posix(), HMI_DIR=hmi.as_posix(),
                   HMI_PI_BUILD=build.as_posix(), HMI_PI_TESTS=' '.join(tests), HMI_HOST='fake-pi',
                   TRACE=(root / 'trace').as_posix())
        return run_bash(script, env), home / '.local'

    def test_binaries_staged_then_swapped_only_after_tests_pass(self):
        result, pi = self.ship()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ran = (pi.parent / 'ran').read_text().splitlines()
        self.assertEqual(ran[:2], ['ran ksphmi_tests', 'ran ksphmi_render_tests'])
        self.assertIn('python3 -B tests/test_setup_kiosk.py', ran)  # the fake, never this machine's python3
        self.assertIn('python3 tests/test_screenshots.py', ' '.join(ran).replace('-B ', ''))
        self.assertIn('ran ksphmi', (pi / 'bin/ksphmi').read_text())
        self.assertEqual((pi / 'bin/ksphmi.prev').read_text(), 'old binary')
        self.assertFalse((pi / 'share/ksphmi/build').exists())  # test programs are not installed
        self.assertEqual((pi / 'share/ksphmi/config.toml').read_text(), 'new config.toml')
        self.assertTrue((pi / 'share/ksphmi/systemd/file.txt').is_file())
        self.assertFalse((pi / 'share/ksphmi.next').exists())
        self.assertFalse((pi / 'share/ksphmi.old').exists())

    def test_failed_pi_test_leaves_the_running_hmi_untouched(self):
        result, pi = self.ship(failing_test='ksphmi_render_tests')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('DIE tests failed', result.stdout)
        self.assertEqual((pi / 'bin/ksphmi').read_text(), 'old binary')
        self.assertEqual((pi / 'share/ksphmi/config.toml').read_text(), 'old config')
        self.assertFalse((pi / 'bin/ksphmi.prev').exists())
        self.assertTrue((pi / 'share/ksphmi.next/build/ksphmi').is_file())  # kept for inspection

    def test_sysroot_copied_again_only_when_the_pi_packages_change(self):
        stubs = self.STUBS + '''hmi_ssh() {
  case "$1" in
    *dpkg-query*) printf '%s  -\\n' "$PACKAGES" ;;
    *tar*) record copy; cat >/dev/null ;;
  esac
}
hmi_sysroot_unpack() { record unpack; mkdir -p "$HMI_PI_SYSROOT"; }
'''
        script = stubs + ci_functions('hmi_sysroot_stamp', 'hmi_sysroot') + 'hmi_sysroot "$@"\n'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'hmi-cpp/cmake').mkdir(parents=True)
            (root / 'hmi-cpp/cmake/pi-sysroot-files.sh').write_text('echo usr/include\n', newline='\n')
            trace = root / 'trace'
            env = dict(os.environ, TRACE=trace.as_posix(), HMI_DIR=(root / 'hmi-cpp').as_posix(),
                       HMI_PI_SYSROOT=(root / 'pi-sysroot').as_posix(), HMI_HOST='fake-pi')
            for packages, extra, copies in (('aaa', [], 1), ('aaa', [], 1), ('bbb', [], 2), ('bbb', ['--force'], 3)):
                result = run_bash(script + '', dict(env, PACKAGES=packages)) if not extra else \
                    subprocess.run([find_bash(), '--noprofile', '--norc', '-c', script, 'ci'] + extra,
                                   env=dict(env, PACKAGES=packages), capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                calls = trace.read_text().splitlines() if trace.exists() else []
                self.assertEqual(calls.count('copy'), copies, (packages, extra, result.stdout))
                self.assertEqual(calls.count('unpack'), copies)
            self.assertIn('packages bbb list', (root / 'pi-sysroot/.stamp').read_text())

    def test_one_deploy_at_a_time_on_the_pi(self):
        stubs = self.STUBS + '''hmi_ssh() { HOME="$FAKE_HOME" bash -c "$1"; }
'''
        script = stubs + ci_functions('hmi_deploy_lock', 'hmi_deploy_unlock')
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            env = dict(os.environ, FAKE_HOME=home.as_posix(), HMI_HOST='fake-pi')
            first = run_bash(script + 'hmi_deploy_lock\necho held\n', env)  # exits while holding: the trap releases
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertFalse((home / 'dev/run/ksphmi/.deploy.lock').exists())
            (home / 'dev/run/ksphmi/.deploy.lock').mkdir()
            (home / 'dev/run/ksphmi/.deploy.lock/owner').write_text('Mac pid 1\n')
            busy = run_bash(script + 'hmi_deploy_lock\n', env)
            self.assertNotEqual(busy.returncode, 0)
            self.assertIn('another ./ci deploy_hmi is running', busy.stdout)
            self.assertIn('Mac pid 1', busy.stdout)
            self.assertTrue((home / 'dev/run/ksphmi/.deploy.lock').exists())  # never removes another deploy's lock
            old = 1_000_000_000
            os.utime(home / 'dev/run/ksphmi/.deploy.lock', (old, old))
            stale = run_bash(script + 'hmi_deploy_lock\nhmi_deploy_unlock\n', env)
            self.assertEqual(stale.returncode, 0, stale.stdout + stale.stderr)
            self.assertIn('older than 30 minutes', stale.stdout)
            for run in (first, busy, stale):  # stat/date flags must work on GNU and BSD (macOS)
                self.assertNotIn('syntax error', run.stdout + run.stderr)
                self.assertNotIn('illegal option', run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
