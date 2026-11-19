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
        self.assertIn('Bash(python3 /repo/tools/agent_mailbox.py --config /repo/build/agent-mailbox/pi-claude.json:*)',
                      rules)  # absolute tools-home paths, also on the Pi
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
        py = '/repo/build/venv/Scripts/python.exe'
        self.assertEqual(p['py'], py)
        for command in (py + ' -m unittest tools/test_agent_daemon.py', py + ' tools/verify.py --docs-only',
                        py + ' /repo/tools/agent_mailbox.py --config /repo/build/agent-mailbox/pc-claude.json pending',
                        'git status', 'git log -3', 'git -C /repo diff main', 'git pull --ff-only'):
            self.assertTrue(allows(rules, denied, command), command)
        for command in ('python3 -m unittest x', py + ' tools/verify.py', 'systemctl --user restart x',
                        'schtasks /Create /TN x', 'launchctl bootstrap gui/501 x', 'git commit -m x',
                        'git -C /repo commit -m x', 'git checkout main', py + ' tools/agent_daemon_install.py '
                        '--identity pc-claude install', 'powershell -c x'):
            self.assertFalse(allows(rules, denied, command), command)
        self.assertIn('PowerShell', denied)
        mac_rules, _, mac = profile('workstation', 'macos', 'mac-claude')
        self.assertEqual(mac['py'], 'python3')

    def test_git_c_only_read_only_forms_on_the_main_checkout(self):
        for name in daemon.PROFILES:
            allowed, denied, _ = profile(name)
            self.assertNotIn('Bash(git -C:*)', allowed)
            no_deny = []  # the allowlist alone must refuse these, whether or not mid-rule * denies work
            for command in ('git -C /repo push', 'git -C /repo push origin main', 'git -C /repo merge x',
                            'git -C /repo reset --hard', 'git -C /repo commit -m x', 'git -C /repo checkout main',
                            'git -C /repo/.claude/worktrees/x status', 'git -C /repo/.claude/worktrees/x push',
                            'git -C /elsewhere status'):
                self.assertFalse(allows(allowed, no_deny, command), (name, command))
            for command in ('git -C /repo status', 'git -C /repo log -3', 'git -C /repo diff main',
                            'git -C /repo show HEAD', 'git -C /repo branch -a', 'git -C /repo fetch',
                            'git -C /repo worktree list', 'git -C /repo worktree add .claude/worktrees/x -b agent/x'):
                self.assertTrue(allows(allowed, denied, command), (name, command))
            pull = allows(allowed, denied, 'git -C /repo pull --ff-only')
            self.assertEqual(pull, name == 'pi', name)
        self.assertEqual(daemon.git_c_rules(r'C:\Users\a\wk\K')[0], 'git -C C:/Users/a/wk/K status:*')

    def test_run_prompt_names_the_git_c_forms(self):
        text = daemon.RUN_PROMPT.format(identity=ME, ids='#1', repo='/repo', tools='/repo', mailbox='mb',
                                        limits='x', gitc='/repo', pull='')
        self.assertIn('git -C /repo <status|log|diff|show|branch|fetch|worktree list|worktree add>', text)
        self.assertIn('`git -C` into a worktree is refused', text)

    def test_rule_paths_for_windows_and_posix(self):
        self.assertEqual(daemon.rule_path(r'C:\Users\a\wk\K'), '//c/Users/a/wk/K')
        self.assertEqual(daemon.rule_path('/home/a/wk/K'), '//home/a/wk/K')

    def test_run_argv_carries_both_lists(self):
        _, _, p = profile('workstation', 'windows', 'pc-claude')
        argv = daemon.run_argv('prompt', p, Path('/repo'), 'c.json')
        self.assertEqual(argv[1:5], ['-p', 'prompt', '--permission-mode', 'dontAsk'])
        self.assertIn('Bash(git push:*)', argv[argv.index('--disallowedTools'):])
        self.assertIn('Edit(//repo/.claude/worktrees/**)', argv[argv.index('--allowedTools'):])


def foreign(identity, tools, work, system):
    config = (tools.rstrip('/') + '/build/agent-mailbox/%s.json') % identity
    p = daemon.load_profile(work, config, identity, system=system, tools=tools)
    return daemon.allowed_tools(work, config, p), p['deny'], p


FOREIGN = [('pc-workbench', 'C:/Users/alanl/wk/KSPKontroller', 'C:/Users/alanl/wk/decompilation/ksp-workbench',
            'windows'),
           ('pc-redux', 'C:/Users/alanl/wk/KSPKontroller', 'C:/Users/alanl/wk/KSP2_decompilation', 'windows'),
           ('mac-workbench', '/Users/alan/wk/KSPKontroller', '/Users/alan/wk/ksp-workbench', 'macos')]


class ForeignRepoTest(unittest.TestCase):
    def test_foreign_repo_rules_point_to_the_work_repo_and_the_mailbox_to_the_tools_home(self):
        for identity, tools, work, system in FOREIGN:
            allowed, denied, p = foreign(identity, tools, work, system)
            self.assertEqual(p['name'], 'foreign', identity)
            py = tools + '/build/venv/Scripts/python.exe' if system == 'windows' else 'python3'
            mailbox = '%s %s/tools/agent_mailbox.py --config %s/build/agent-mailbox/%s.json' % (py, tools, tools,
                                                                                               identity)
            self.assertEqual(p['mailbox'], mailbox)
            self.assertIn('Edit(%s/.claude/worktrees/**)' % daemon.rule_path(work), allowed)
            self.assertFalse(any(r.startswith('Edit(') and 'KSPKontroller' in r for r in allowed), identity)
            for command in (mailbox + ' pending', mailbox + ' send --to pc-claude --id x --text RESULT',
                            'git -C %s status' % work, 'git -C %s log -3' % work, 'git status', 'git diff main'):
                self.assertTrue(allows(allowed, denied, command), (identity, command))
            for command in FORBIDDEN + [
                    'git -C %s push' % work, 'git -C %s pull --ff-only' % work, 'git pull --ff-only',
                    'git -C %s status' % tools, 'git -C %s/.claude/worktrees/x status' % work,
                    py + ' -m unittest discover', 'python3 -m pytest', py + ' tools/agent_mailbox.py --config x pending',
                    '%s %s/tools/agent_daemon_install.py --identity %s install' % (py, tools, identity),
                    'dotnet build', 'make', 'git commit -m x']:
                self.assertFalse(allows(allowed, denied, command), (identity, command))
            self.assertIn('Read(%s/build/agent-mailbox/*.json)' % daemon.rule_path(tools), denied)
            self.assertIn('Read(%s/build/agent-mailbox/*.json)' % daemon.rule_path(work), denied)
            prompt = daemon.run_prompt(identity, '#1', p)
            self.assertIn('Work in %s.' % work, prompt)
            self.assertIn('Read %s/CLAUDE.md' % tools, prompt)
            self.assertIn('`%s pending`' % mailbox, prompt)
            self.assertNotIn(' tools/agent_mailbox.py', prompt)
            self.assertIn('git -C %s <status' % work, prompt)

    def test_tests_only_when_the_repo_declares_them(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d)
            tools = '/Users/alan/wk/KSPKontroller'
            allowed, denied, p = foreign('mac-workbench', tools, work, 'macos')
            self.assertFalse(allows(allowed, denied, 'python3 -m unittest discover tests'))
            self.assertIsNone(p['profile_file'])
            (work / '.claude').mkdir()
            (work / '.claude' / 'agent-daemon.json').write_text(json.dumps(
                {'allow': ['python3 -m unittest discover tests:*'], 'deny': ['Bash(./deploy.sh:*)']}), encoding='utf-8')
            allowed, denied, p = foreign('mac-workbench', tools, work, 'macos')
            self.assertTrue(allows(allowed, denied, 'python3 -m unittest discover tests -v'))
            self.assertFalse(allows(allowed, denied, './deploy.sh'))
            self.assertTrue(p['profile_file'].endswith('agent-daemon.json'))
            for rule in daemon.BASE_DENY:
                self.assertIn(rule, denied)

    def test_same_repo_keeps_the_identity_default(self):
        self.assertEqual(daemon.load_profile('/repo', 'c.json', 'pc-claude', tools='/repo/')['name'], 'workstation')
        self.assertEqual(daemon.load_profile('C:/K', 'c.json', 'pc-claude', tools=r'c:\K')['name'], 'workstation')
        self.assertEqual(daemon.load_profile('/repo', 'c.json', 'pi-claude')['name'], 'pi')

    def test_run_uses_the_work_repo_as_cwd_and_the_tools_home_for_the_mailbox(self):
        with tempfile.TemporaryDirectory() as d:
            tools, work = Path(d) / 'tools', Path(d) / 'work'
            (tools / 'build' / 'agent-mailbox').mkdir(parents=True)
            work.mkdir()
            config = tools / 'build' / 'agent-mailbox' / 'pc-workbench.json'
            config.write_text(json.dumps({'peer': 'pc-workbench'}), encoding='utf-8')
            calls = []

            def fake_run(argv, **kw):
                calls.append((argv, kw.get('cwd')))
                if 'stdout' in kw and hasattr(kw['stdout'], 'write'):
                    kw['stdout'].write('SUMMARY: ok\n')
                return subprocess.CompletedProcess(argv, 0, '', '')

            old = (daemon.TOOLS, daemon.WORK, daemon.MAILBOX)
            try:
                with mock.patch.object(daemon, 'run', side_effect=lambda args: None):
                    daemon.main(['--tools-dir', str(tools), '--repo', str(work), '--config',
                                 'build/agent-mailbox/pc-workbench.json', 'run'])
                self.assertEqual((daemon.TOOLS, daemon.WORK), (tools, work))
                self.assertEqual(daemon.MAILBOX, tools / 'tools' / 'agent_mailbox.py')
                self.assertEqual(daemon.resolve_config('build/agent-mailbox/pc-workbench.json'), config.resolve())
                p = daemon.load_profile(work, config, 'pc-workbench', tools=tools, system='windows')
                f = daemon.files(config)
                with mock.patch.object(daemon.subprocess, 'run', side_effect=fake_run), \
                        mock.patch.object(daemon, 'record_health'), mock.patch.object(daemon, 'log'):
                    daemon.launch(config, 'pc-workbench', [msg('m1', recipient='pc-workbench')], f, {}, p)
                    daemon.mailbox(config, 'pending')
                self.assertEqual(calls[0][1], work)  # claude -p runs in the work repo
                self.assertIn('Edit(%s/**)' % daemon.rule_path(work / '.claude' / 'worktrees'), calls[0][0])
                self.assertEqual(calls[1][1], tools)  # the mailbox client runs from the tools home
                self.assertEqual(calls[1][0][1], str(tools / 'tools' / 'agent_mailbox.py'))
            finally:
                daemon.TOOLS, daemon.WORK, daemon.MAILBOX = old

    def test_a_config_in_the_work_repo_is_found_after_the_tools_home(self):
        with tempfile.TemporaryDirectory() as d:
            tools, work = Path(d) / 'tools', Path(d) / 'work'
            (work / 'build' / 'agent-mailbox').mkdir(parents=True)
            tools.mkdir()
            config = work / 'build' / 'agent-mailbox' / 'mac-workbench.json'
            config.write_text('{}', encoding='utf-8')
            old = (daemon.TOOLS, daemon.WORK)
            try:
                daemon.TOOLS, daemon.WORK = tools, work
                self.assertEqual(daemon.resolve_config('build/agent-mailbox/mac-workbench.json'), config.resolve())
            finally:
                daemon.TOOLS, daemon.WORK = old


class InstallerTest(unittest.TestCase):
    def dry(self, system, action='install', identity='pc-claude', repo=None, tools=None, extra=()):
        out = []
        with mock.patch('builtins.print', side_effect=lambda *a, **k: out.append(' '.join(map(str, a)))), \
                mock.patch.object(installer, 'apply', side_effect=AssertionError('dry run applied')), \
                mock.patch.object(installer.subprocess, 'run', side_effect=AssertionError('dry run ran')):
            argv = ['--identity', identity, '--os', system, action, '--dry-run']
            if repo:
                argv[0:0] = ['--repo', repo]
            if tools:
                argv[0:0] = ['--tools-dir', tools]
            argv[0:0] = list(extra)
            self.assertEqual(installer.main(argv), 0)
        return '\n'.join(out)

    def test_linux_keeps_the_systemd_unit(self):
        plan = installer.make_plan('linux', 'pi-claude', '/home/pi/dev/ksp1/KSPKontroller', '/home/pi')
        self.assertEqual([p for p, _ in plan['files']], ['/home/pi/.local/lib/agent-daemon/agent_daemon.py',
                                                         '/home/pi/.config/systemd/user/agent-daemon@.service'])
        unit = plan['files'][1][1]
        self.assertEqual(unit, installer.UNIT_TEMPLATE.read_text(encoding='utf-8'))  # universal newlines: LF
        text = self.dry('linux', identity='pi-claude', tools='/home/pi/dev/ksp1/KSPKontroller')
        self.assertIn('ExecStart=/usr/bin/python3 %h/.local/lib/agent-daemon/agent_daemon.py', text)
        self.assertNotIn('.service.d', text.split('run systemctl')[0])  # Pi: tools home = work repo, no drop-in
        self.assertIn('run systemctl --user enable --now agent-daemon@pi-claude.service', text)
        self.assertIn('copy /home/pi/dev/ksp1/KSPKontroller/tools/agent_daemon.py -> ', text)
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
        text = self.dry('macos', 'uninstall', 'mac-claude', tools='/Users/a/wk/KSPKontroller')
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
        text = self.dry('windows', 'uninstall', 'pc-claude', tools=repo)
        self.assertIn(r'schtasks /Delete /TN \KSPKontroller\agent-daemon-pc-claude /F', text)

    def test_pc_workbench_runs_in_its_repo_with_kspkontroller_tools(self):
        tools, work = r'C:\Users\alanl\wk\KSPKontroller', 'C:/Users/alanl/wk/decompilation/ksp-workbench'
        text = self.dry('windows', identity='pc-workbench', repo=work, tools='C:/Users/alanl/wk/KSPKontroller')
        for piece in (r'<Command>C:\Users\alanl\wk\KSPKontroller\build\venv\Scripts\pythonw.exe</Command>',
                      r'"C:\Users\alanl\wk\KSPKontroller\tools\agent_daemon.py" --config '
                      r'"C:\Users\alanl\wk\KSPKontroller\build\agent-mailbox\pc-workbench.json" '
                      r'--repo "C:\Users\alanl\wk\decompilation\ksp-workbench" run --max-runs 6',
                      r'<WorkingDirectory>C:\Users\alanl\wk\decompilation\ksp-workbench</WorkingDirectory>',
                      tools + r'\build\agent-mailbox\pc-workbench.daemon\daemon.log',
                      tools + r'\build\agent-mailbox\pc-workbench.daemon\task.xml'):
            self.assertIn(piece, text)
        self.assertNotIn(r'ksp-workbench\build', text)
        self.assertNotIn(r'ksp-workbench\tools', text)

    def test_pc_redux_and_a_custom_config(self):
        plan = installer.make_plan('windows', 'pc-redux', r'C:\Users\alanl\wk\KSPKontroller', '', user='H\a',
                                   work='C:/Users/alanl/wk/KSP2_decompilation')
        task = plan['files'][0][1][1]
        self.assertIn(r'<WorkingDirectory>C:\Users\alanl\wk\KSP2_decompilation</WorkingDirectory>', task)
        self.assertIn(r'--repo "C:\Users\alanl\wk\KSP2_decompilation"', task)
        self.assertIn(r'KSPKontroller\build\agent-mailbox\pc-redux.json', task)
        same = installer.make_plan('windows', 'pc-claude', r'C:\K', '', user='H\a')
        self.assertNotIn('--repo', same['files'][0][1][1])  # tools home = work repo: today's arguments

    def test_mac_workbench_plist(self):
        text = self.dry('macos', identity='mac-workbench', repo='/Users/alan/wk/ksp-workbench',
                        tools='/Users/alan/wk/KSPKontroller')
        plan = installer.make_plan('macos', 'mac-workbench', '/Users/alan/wk/KSPKontroller', '/Users/alan', uid=501,
                                   work='/Users/alan/wk/ksp-workbench')
        data = plistlib.loads(plan['files'][0][1].encode('utf-8'))
        self.assertEqual(data['ProgramArguments'][1:], [
            '/Users/alan/wk/KSPKontroller/tools/agent_daemon.py', '--config',
            '/Users/alan/wk/KSPKontroller/build/agent-mailbox/mac-workbench.json', '--repo',
            '/Users/alan/wk/ksp-workbench', 'run', '--max-runs', '6'])
        self.assertEqual(data['WorkingDirectory'], '/Users/alan/wk/ksp-workbench')
        self.assertEqual(data['EnvironmentVariables']['AGENT_DAEMON_REPO'], '/Users/alan/wk/KSPKontroller')
        self.assertEqual(data['EnvironmentVariables']['AGENT_DAEMON_WORK_REPO'], '/Users/alan/wk/ksp-workbench')
        self.assertEqual(data['StandardOutPath'],
                         '/Users/alan/wk/KSPKontroller/build/agent-mailbox/mac-workbench.daemon/daemon.log')
        self.assertIn('tools home: /Users/alan/wk/KSPKontroller', text)
        self.assertIn('work repo:  /Users/alan/wk/ksp-workbench', text)

    def test_config_resolution_order(self):
        tools, work = '/t', '/w'
        found = lambda *paths: (lambda p: p in paths)
        cfg = lambda exists, option=None: installer.resolve_config('macos', 'mac-workbench', tools, work, option,
                                                                   exists=exists)
        self.assertEqual(cfg(found(), '/x/c.json'), (installer.PurePosixPath('/x/c.json'), '--config'))
        both = found('/t/build/agent-mailbox/mac-workbench.json', '/w/build/agent-mailbox/mac-workbench.json')
        self.assertEqual(cfg(both)[1], 'tools home')
        path, source = cfg(found('/w/build/agent-mailbox/mac-workbench.json'))
        self.assertEqual((str(path), source), ('/w/build/agent-mailbox/mac-workbench.json', 'work repo'))
        self.assertTrue(cfg(found())[1].startswith('none found'))
        plan = installer.make_plan('macos', 'mac-workbench', tools, '/Users/alan', uid=501, work=work,
                                   config=str(path))
        data = plistlib.loads(plan['files'][0][1].encode('utf-8'))
        self.assertIn('/w/build/agent-mailbox/mac-workbench.json', data['ProgramArguments'])
        self.assertEqual(data['StandardOutPath'], '/w/build/agent-mailbox/mac-workbench.daemon/daemon.log')

    def test_linux_work_repo_goes_into_the_identity_drop_in(self):
        plan = installer.make_plan('linux', 'pi-claude', '/home/pi/wk/KSPKontroller', '/home/pi')
        self.assertEqual(len(plan['files']), 2)
        plan = installer.make_plan('linux', 'pi-x', '/home/pi/wk/KSPKontroller', '/home/pi', work='/home/pi/wk/other')
        path, dropin = plan['files'][2]
        self.assertEqual(path, '/home/pi/.config/systemd/user/agent-daemon@pi-x.service.d/work-repo.conf')
        for piece in ('WorkingDirectory=/home/pi/wk/other', 'Environment=AGENT_DAEMON_WORK_REPO=/home/pi/wk/other',
                      'Environment=AGENT_DAEMON_REPO=/home/pi/wk/KSPKontroller', '\nExecStart=\n',
                      '--repo /home/pi/wk/other run --max-runs 6'):
            self.assertIn(piece, dropin)
        self.assertIn(path, plan['remove'])

    def test_a_real_install_refuses_missing_paths(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d) / 'work'
            work.mkdir()
            err = io.StringIO()
            with mock.patch.object(installer, 'apply', side_effect=AssertionError('applied')), \
                    contextlib.redirect_stderr(err):
                rc = installer.main(['--identity', 'pc-workbench', '--tools-dir', str(Path(d) / 'tools'),
                                     '--python', str(Path(d) / 'nopython'), '--repo', str(work), 'install'])
            self.assertEqual(rc, 2)
            for what in ('python', 'daemon', 'mailbox client', 'config'):
                self.assertIn('refusing to install: %s ' % what, err.getvalue())
            self.assertNotIn('work repo', err.getvalue())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(installer.main(['--identity', 'pc-workbench', '--tools-dir', str(Path(d) / 'tools'),
                                                 '--repo', str(work), 'install', '--dry-run']), 0)
            self.assertIn('missing: config ', out.getvalue())
            self.assertIn('none found', out.getvalue())

    def test_a_real_install_proceeds_when_every_path_exists(self):
        with tempfile.TemporaryDirectory() as d:
            tools = Path(d) / 'tools'
            for rel in ('tools/agent_daemon.py', 'tools/agent_mailbox.py', 'build/agent-mailbox/x-y.json'):
                (tools / rel).parent.mkdir(parents=True, exist_ok=True)
                (tools / rel).write_text('', encoding='utf-8')
            python = tools / 'py.exe'
            python.write_text('', encoding='utf-8')
            with mock.patch.object(installer, 'apply', return_value=0) as applied:
                self.assertEqual(installer.main(['--identity', 'x-y', '--tools-dir', str(tools), '--python',
                                                 str(python), 'install']), 0)
            applied.assert_called_once()

    def test_a_real_install_for_another_os_is_refused(self):
        other = 'macos' if installer.platform_name() != 'macos' else 'windows'
        with self.assertRaises(SystemExit), mock.patch('sys.stderr'):
            installer.main(['--identity', 'pc-claude', '--os', other, 'install'])


if __name__ == '__main__':
    unittest.main()
