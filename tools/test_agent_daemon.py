"""Tests for tools/agent_daemon.py and tools/agent_daemon_install.py: wake rule, pause, health check, tool profiles
and service installers (no model, no mailbox server, nothing installed). Runs on Windows, macOS and Linux."""

import contextlib
import fnmatch
import io
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
from xml.dom import minidom
import time
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


daemon = load('agent_daemon')
installer = load('agent_daemon_install')
ME = 'pi-claude'


def msg(mid, kind='REQUEST', text='REQUEST: do it', recipient=ME, **extra):
    return dict(id=mid, seq=1, kind=kind, sender='pc-claude', text=text, to=[recipient], recipient=recipient, **extra)


def matches(rule, command):
    """Claude Code Bash rule semantics: Bash(x:*) is a prefix match, * elsewhere a wildcard, else exact."""
    if not (rule.startswith('Bash(') and rule.endswith(')')):
        return False
    body = rule[5:-1]
    if body.endswith(':*'):
        body = body[:-2] + '*'
    if '*' in body:
        return fnmatch.fnmatchcase(command, body)
    return command == body


def allows(allowed, denied, command):
    """A deny wins over an allow; under dontAsk anything not allowed is refused."""
    return not any(matches(r, command) for r in denied) and any(matches(r, command) for r in allowed)


def profile(name, system='linux', identity=ME):
    config = 'build/agent-mailbox/%s.json' % identity
    p = daemon.load_profile(Path('/repo'), config, identity, name, system=system)
    return daemon.allowed_tools(Path('/repo'), config, p), p['deny'], p


FORBIDDEN = ['./ci deploy', './ci deploy_all', './ci deploy_hmi', './ci install', './ci upload_sketch',
             './ci.cmd deploy', 'ci.cmd upload_sketch', 'bash ci deploy', 'git push', 'git push origin main',
             'git -C /repo push origin main', 'git -C /repo/.claude/worktrees/x push', 'git merge agent/x',
             'git -C /repo merge agent/x', 'git reset --hard', 'git -C /repo reset --hard', 'sudo reboot',
             'arduino-cli upload', 'rm -rf build', 'kill 1', 'pkill ksphmi']


class WakeTest(unittest.TestCase):
    def test_request_and_notice_wake(self):
        self.assertIs(daemon.select([msg('a')], {}, 6, time.time(), ME), True)
        self.assertIs(daemon.select([msg('n', 'NOTICE', 'NOTICE: reassigned')], {}, 6, time.time(), ME), True)

    def test_info_ack_and_result_never_wake(self):
        for kind, text in (('INFO', 'GO'), ('ACK', 'ACK #1'), (None, 'ACK #1 (protocol 3)'), ('RESULT', 'RESULT #3'),
                           (None, 'just a note')):
            self.assertIs(daemon.select([msg('a', kind, text)], {}, 6, time.time(), ME), False, text)

    def test_legacy_items_take_the_kind_from_the_text(self):
        self.assertIs(daemon.select([msg('a', None, 'request: legacy')], {}, 6, time.time(), ME), True)

    def test_addressing_identity_group_and_all(self):
        for address in (ME, 'pi', 'all', 'mac-claude,pi', 'pc-claude, pi-claude'):
            self.assertTrue(daemon.wakes(msg('a', address=address), ME), address)
        for address in ('pc', 'mac-claude', 'pc-claude,mac'):
            self.assertFalse(daemon.wakes(msg('a', address=address), ME), address)
        self.assertTrue(daemon.wakes(msg('a', recipient='pi'), ME))  # legacy host inbox
        self.assertFalse(daemon.wakes(msg('a', recipient='pc-claude'), ME))
        self.assertTrue(daemon.wakes(dict(id='a', kind='REQUEST', to=['mac-claude', ME]), ME))
        self.assertFalse(daemon.wakes(dict(id='a', kind='REQUEST', to=['mac-claude']), ME))

    def test_a_seen_waiting_request_does_not_rerun_and_info_does_not_revive_it(self):
        state = {'seen': {'a': 'x'}}
        self.assertIs(daemon.select([msg('a')], state, 6, time.time(), ME), False)
        self.assertIs(daemon.select([msg('a'), msg('go', 'INFO', 'GO')], state, 6, time.time(), ME), False)
        self.assertIs(daemon.select([msg('a'), msg('go', 'REQUEST', 'REQUEST: GO')], state, 6, time.time(), ME),
                      True)

    def test_three_failures_give_up(self):
        self.assertIs(daemon.select([msg('a')], {'failures': {'a': 2}}, 6, time.time(), ME), True)
        self.assertIs(daemon.select([msg('a')], {'failures': {'a': 3}}, 6, time.time(), ME), False)

    def test_rate_limit_counts_the_last_hour_only(self):
        now = time.time()
        self.assertEqual(daemon.select([msg('a')], {'runs': [now - 60] * 6}, 6, now, ME), 'rate-limited')
        self.assertIs(daemon.select([msg('a')], {'runs': [now - 4000] * 6}, 6, now, ME), True)


class CycleTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.f = daemon.files(Path(self.dir.name) / 'pi-claude.json')
        self.mb = mock.Mock()

    def tearDown(self):
        self.dir.cleanup()

    def cycle(self, items):
        ok = subprocess.CompletedProcess([], 0, '', '')
        with mock.patch.object(daemon, 'mailbox', return_value=ok) as mailbox, \
                mock.patch.object(daemon, 'pending', return_value=(self.mb, {}, items)), \
                mock.patch.object(daemon, 'launch') as launch:
            daemon.cycle('cfg.json', ME, self.f, 6, {'name': 'pi'})
        return mailbox, launch

    def test_paused_daemon_does_not_collect(self):
        daemon.pause(self.f, 5)
        mailbox, launch = self.cycle([msg('a')])
        mailbox.assert_not_called()
        launch.assert_not_called()
        daemon.resume(self.f)
        mailbox, launch = self.cycle([msg('a')])
        mailbox.assert_called_once_with('cfg.json', 'collect')
        launch.assert_called_once()

    def test_info_only_inbox_spends_no_model(self):
        _, launch = self.cycle([msg('i', 'INFO', 'INFO: fyi'), msg('r', 'RESULT', 'RESULT #2 done')])
        launch.assert_not_called()

    def test_acks_are_marked_done_without_a_model(self):
        _, launch = self.cycle([msg('k', 'ACK', 'ACK #1')])
        self.mb.mark_done.assert_called_once_with({}, 'cfg.json', message_id='k')
        launch.assert_not_called()

    def test_a_run_gets_info_as_context_too(self):
        _, launch = self.cycle([msg('i', 'INFO', 'INFO: fyi'), msg('a')])
        self.assertEqual([i['id'] for i in launch.call_args[0][2]], ['i', 'a'])


class PauseTest(unittest.TestCase):
    def test_pause_expires_and_resume_clears(self):
        with tempfile.TemporaryDirectory() as d:
            f = daemon.files(Path(d) / 'pi-claude.json')
            self.assertFalse(daemon.paused_until(f))
            daemon.write_json(f['pause'], {'until': time.time() + 60})
            self.assertTrue(daemon.paused_until(f))
            daemon.write_json(f['pause'], {'until': time.time() - 1})
            self.assertFalse(daemon.paused_until(f))
            with contextlib.redirect_stdout(io.StringIO()):
                daemon.main(['--config', str(Path(d) / 'pi-claude.json'), 'pause', '--minutes', '1'])
            self.assertTrue(daemon.paused_until(f))
            with contextlib.redirect_stdout(io.StringIO()):
                daemon.main(['--config', str(Path(d) / 'pi-claude.json'), 'resume'])
            self.assertFalse(daemon.paused_until(f))
            daemon.resume(f)  # idempotent

    def test_one_daemon_per_identity(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'lock'
            first = daemon.acquire_lock(path)
            self.assertIsNotNone(first)
            # flock is per open file description and msvcrt per handle: a second open is refused on both
            second = daemon.acquire_lock(path)
            self.assertIsNone(second)
            first.close()
            third = daemon.acquire_lock(path)
            self.assertIsNotNone(third)
            third.close()


class HealthTest(unittest.TestCase):
    def finish(self, f, state, rc, output, system='linux'):
        sent = []
        with mock.patch.object(daemon, 'send', side_effect=lambda *a: sent.append(a) or True), \
                mock.patch.object(daemon.subprocess, 'run') as run:
            daemon.finish('cfg.json', ME, [msg('a')], f, state, rc, output, 'r.log', '#1 a', time.time(), system)
        return sent, run

    def test_two_failures_report_once_per_streak_and_success_resets(self):
        with tempfile.TemporaryDirectory() as d:
            f = daemon.files(Path(d) / 'pi-claude.json')
            state = {}
            sent, _ = self.finish(f, state, 1, 'Error: OAuth session expired')
            self.assertEqual(sent, [])
            sent, _ = self.finish(f, state, 1, 'Error: OAuth session expired\n')
            infos = [s for s in sent if s[1] == 'all']
            self.assertEqual(len(infos), 1)
            _, to, message_id, text = infos[0]
            self.assertTrue(message_id.startswith('daemon-unhealthy-pi-claude-'))
            self.assertTrue(text.startswith('INFO: daemon of pi-claude unhealthy:'))
            self.assertIn('OAuth session expired', text)
            self.assertEqual([s[1] for s in sent if s[1] != 'all'], ['pc-claude'])  # Linux: INFO to coordinator
            sent, _ = self.finish(f, state, 'timeout', '')
            self.assertEqual(sent, [])  # same streak: reported already
            self.assertEqual(state['health']['streak'], 3)
            self.finish(f, state, 0, 'SUMMARY: ok')
            self.assertEqual(state['health'], {'streak': 0})
            self.finish(f, state, 1, 'boom')
            sent, _ = self.finish(f, state, 1, 'boom')
            self.assertEqual(len([s for s in sent if s[1] == 'all']), 1)  # a new streak reports again
            self.assertEqual(json.loads(f['state'].read_text(encoding='utf-8'))['health']['streak'], 2)

    def test_needs_user_notifies_locally(self):
        with tempfile.TemporaryDirectory() as d:
            f = daemon.files(Path(d) / 'mac-claude.json')
            sent, run = self.finish(f, {}, 0, 'RESULT #4 needs user: deploy\nSUMMARY: #4 needs user', 'macos')
            self.assertEqual(sent, [])
            argv = run.call_args[0][0]
            self.assertEqual(argv[0], 'osascript')
            self.assertIn('display notification', argv[2])
            sent, run = self.finish(f, {}, 0, 'SUMMARY: #4 needs user', 'windows')
            self.assertEqual(run.call_args[0][0][0], 'powershell.exe')
            self.assertIn('ToastNotificationManager', run.call_args[0][0][-1])
            self.assertIn('msg.exe', run.call_args[0][0][-1])  # fallback
            sent, run = self.finish(f, {}, 0, 'SUMMARY: #4 handled')
            run.assert_not_called()
            self.assertEqual(sent, [])

    def test_notification_failures_are_never_fatal(self):
        with mock.patch.object(daemon.subprocess, 'run', side_effect=OSError('no osascript')):
            daemon.notify_user('cfg', 'mac-claude', 't', 'b', 'x', 'macos')
        with mock.patch.object(daemon, 'mailbox', side_effect=OSError('server down')):
            self.assertFalse(daemon.send('cfg', 'pc-claude', 'x', 'INFO: t'))

    def test_quotes_survive_notification_scripts(self):
        (argv,) = daemon.notify_commands("it's", 'say "hi"', 'windows')
        self.assertIn("'it''s'", argv[-1])
        (argv,) = daemon.notify_commands("it's", 'say "hi"', 'macos')
        self.assertIn('\\"hi\\"', argv[2])


class ProfileTest(unittest.TestCase):
    def test_deploy_push_flash_denied_in_every_profile(self):
        for name in daemon.PROFILES:
            for system in ('linux', 'macos', 'windows'):
                allowed, denied, _ = profile(name, system)
                for command in FORBIDDEN:
                    self.assertFalse(allows(allowed, denied, command), (name, system, command))

    def test_a_profile_file_cannot_lift_the_base_denylist(self):
        with tempfile.TemporaryDirectory() as d:
            extra = Path(d) / 'profile.json'
            extra.write_text(json.dumps({'allow': ['git push:*', './ci:*', 'make test'], 'deny': ['Bash(make:*)']}),
                             encoding='utf-8')
            p = daemon.load_profile(Path('/repo'), 'c.json', 'pc-claude', profile_file=extra, system='windows')
            allowed = daemon.allowed_tools(Path('/repo'), 'c.json', p)
            for command in ('git push', './ci deploy', 'make test'):
                self.assertFalse(allows(allowed, p['deny'], command), command)
            for rule in daemon.BASE_DENY:
                self.assertIn(rule, p['deny'])

    def test_default_profile_by_identity(self):
        self.assertEqual(daemon.default_profile('pi-claude'), 'pi')
        self.assertEqual(daemon.default_profile('pc-claude'), 'workstation')
        self.assertEqual(daemon.default_profile('mac-claude'), 'workstation')
        with self.assertRaises(ValueError):
            daemon.load_profile(Path('/repo'), 'c.json', 'pc-claude', 'root')

    def test_pi_profile_keeps_todays_rules(self):
        rules, denied, _ = profile('pi')
        joined = ' '.join(rules)
        for forbidden in ('sudo', 'git push', 'git merge', './ci', 'cmake', 'make:', 'Agent', 'rm:',
                          'systemctl --user stop', 'systemctl --user start', 'kill'):
            self.assertNotIn(forbidden, joined)
        self.assertEqual([r for r in rules if 'restart' in r], ['Bash(systemctl --user restart agent-mailbox.service)'])
        self.assertIn('Edit(//repo/.claude/worktrees/**)', rules)
        self.assertIn('Bash(python3 tools/agent_mailbox.py --config build/agent-mailbox/pi-claude.json:*)', rules)
        self.assertNotIn('Edit', rules)  # never unrestricted edits
        self.assertTrue(any('build/agent-mailbox/*.json' in r for r in denied))
        self.assertTrue(any(r.endswith('ksphmi/**)') for r in denied))

    def test_only_the_mailbox_server_may_be_restarted_on_the_pi(self):
        rules, denied, _ = profile('pi')
        self.assertTrue(allows(rules, denied, 'systemctl --user restart agent-mailbox.service'))
        self.assertTrue(allows(rules, denied, 'journalctl _SYSTEMD_USER_UNIT=agent-mailbox.service -n 50'))
        for command in ('systemctl --user restart ksphmi.service', 'systemctl --user restart ksphmi',
                        'systemctl --user restart agent-mailbox.service ksphmi.service',
                        'systemctl --user restart agent-daemon@pi-claude.service',
                        'systemctl --user stop agent-mailbox.service', 'systemctl --user start ksphmi.service'):
            self.assertFalse(allows(rules, denied, command), command)

    def test_workstation_is_read_only_plus_tests_and_worktree_edits(self):
        rules, denied, p = profile('workstation', 'windows', 'pc-claude')
        py = 'build/venv/Scripts/python.exe'
        self.assertEqual(p['py'], py)
        for command in (py + ' -m unittest tools/test_agent_daemon.py', py + ' tools/verify.py --docs-only',
                        py + ' tools/agent_mailbox.py --config build/agent-mailbox/pc-claude.json pending',
                        'git status', 'git log -3', 'git -C /repo/.claude/worktrees/x diff', 'git pull --ff-only'):
            self.assertTrue(allows(rules, denied, command), command)
        for command in ('python3 -m unittest x', py + ' tools/verify.py', 'systemctl --user restart x',
                        'schtasks /Create /TN x', 'launchctl bootstrap gui/501 x', 'git commit -m x',
                        'git -C /repo commit -m x', 'git checkout main', py + ' tools/agent_daemon_install.py '
                        '--identity pc-claude install', 'powershell -c x'):
            self.assertFalse(allows(rules, denied, command), command)
        self.assertIn('PowerShell', denied)
        mac_rules, _, mac = profile('workstation', 'macos', 'mac-claude')
        self.assertEqual(mac['py'], 'python3')

    def test_rule_paths_for_windows_and_posix(self):
        self.assertEqual(daemon.rule_path(r'C:\Users\a\wk\K'), '//c/Users/a/wk/K')
        self.assertEqual(daemon.rule_path('/home/a/wk/K'), '//home/a/wk/K')

    def test_run_argv_carries_both_lists(self):
        _, _, p = profile('workstation', 'windows', 'pc-claude')
        argv = daemon.run_argv('prompt', p, Path('/repo'), 'c.json')
        self.assertEqual(argv[1:5], ['-p', 'prompt', '--permission-mode', 'dontAsk'])
        self.assertIn('Bash(git push:*)', argv[argv.index('--disallowedTools'):])
        self.assertIn('Edit(//repo/.claude/worktrees/**)', argv[argv.index('--allowedTools'):])


class InstallerTest(unittest.TestCase):
    def dry(self, system, action='install', identity='pc-claude', repo=None):
        out = []
        with mock.patch('builtins.print', side_effect=lambda *a, **k: out.append(' '.join(map(str, a)))), \
                mock.patch.object(installer, 'apply', side_effect=AssertionError('dry run applied')), \
                mock.patch.object(installer.subprocess, 'run', side_effect=AssertionError('dry run ran')):
            argv = ['--identity', identity, '--os', system, action, '--dry-run']
            if repo:
                argv[0:0] = ['--repo', repo]
            self.assertEqual(installer.main(argv), 0)
        return '\n'.join(out)

    def test_linux_keeps_the_systemd_unit(self):
        plan = installer.make_plan('linux', 'pi-claude', '/home/pi/wk/KSPKontroller', '/home/pi')
        self.assertEqual([p for p, _ in plan['files']], ['/home/pi/.local/lib/agent-daemon/agent_daemon.py',
                                                         '/home/pi/.config/systemd/user/agent-daemon@.service'])
        unit = plan['files'][1][1]
        self.assertEqual(unit, installer.UNIT_TEMPLATE.read_text(encoding='utf-8'))  # universal newlines: LF
        text = self.dry('linux', identity='pi-claude', repo='/home/pi/wk/KSPKontroller')
        self.assertIn('ExecStart=/usr/bin/python3 %h/.local/lib/agent-daemon/agent_daemon.py', text)
        self.assertIn('run systemctl --user enable --now agent-daemon@pi-claude.service', text)
        self.assertIn('copy /home/pi/wk/KSPKontroller/tools/agent_daemon.py -> ', text)
        self.assertIn('disable --now agent-daemon@pi-claude.service', self.dry('linux', 'uninstall', 'pi-claude'))

    def test_macos_launch_agent(self):
        plan = installer.make_plan('macos', 'mac-claude', '/Users/a/wk/KSPKontroller', '/Users/a', uid=501)
        path, plist = plan['files'][0]
        self.assertEqual(path, '/Users/a/Library/LaunchAgents/com.kspkontroller.agent-daemon.mac-claude.plist')
        data = plistlib.loads(plist.encode('utf-8'))
        self.assertEqual(data['ProgramArguments'][1:4], ['/Users/a/wk/KSPKontroller/tools/agent_daemon.py',
                                                         '--config',
                                                         '/Users/a/wk/KSPKontroller/build/agent-mailbox/mac-claude.json'])
        self.assertTrue(data['RunAtLoad'])
        self.assertEqual(data['KeepAlive'], {'SuccessfulExit': False})
        self.assertEqual(plan['install'], [['launchctl', 'bootstrap', 'gui/501', path]])
        text = self.dry('macos', 'uninstall', 'mac-claude', '/Users/a/wk/KSPKontroller')
        self.assertIn('launchctl bootout gui/501/com.kspkontroller.agent-daemon.mac-claude', text)

    def test_windows_scheduled_task(self):
        repo = r'C:\Users\a\wk\KSPKontroller'
        plan = installer.make_plan('windows', 'pc-claude', repo, '', user='HOST\\a')
        path, (encoding, task) = plan['files'][0]
        self.assertEqual(encoding, 'utf-16')
        self.assertIn(r'build\agent-mailbox\pc-claude.daemon\task.xml', path)
        for piece in ('<LogonTrigger>', '<RestartOnFailure>', '<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>',
                      r'C:\Users\a\wk\KSPKontroller\build\venv\Scripts\pythonw.exe', '--log "',
                      r'build\agent-mailbox\pc-claude.json', '<UserId>HOST\\a</UserId>'):
            self.assertIn(piece, task)
        minidom.parseString(task.replace('encoding="UTF-16"', 'encoding="UTF-8"').encode('utf-8'))
        self.assertEqual(plan['install'][0][:4], ['schtasks', '/Create', '/TN', r'\KSPKontroller\agent-daemon-pc-claude'])
        text = self.dry('windows', 'uninstall', 'pc-claude', repo)
        self.assertIn(r'schtasks /Delete /TN \KSPKontroller\agent-daemon-pc-claude /F', text)

    def test_a_real_install_for_another_os_is_refused(self):
        other = 'macos' if installer.platform_name() != 'macos' else 'windows'
        with self.assertRaises(SystemExit), mock.patch('sys.stderr'):
            installer.main(['--identity', 'pc-claude', '--os', other, 'install'])


if __name__ == '__main__':
    unittest.main()
