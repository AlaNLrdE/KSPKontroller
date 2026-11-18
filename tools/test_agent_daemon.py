"""Tests for tools/agent_daemon.py run selection, pause and permission rules (no model, no mailbox server)."""

import importlib.util
import os
from pathlib import Path
import tempfile
import time
import unittest

try:  # the daemon is a Pi (Linux) service: it locks with fcntl, which Windows lacks
    import fcntl  # noqa: F401
except ImportError:
    raise unittest.SkipTest('agent_daemon.py runs on Linux only (fcntl)')

spec = importlib.util.spec_from_file_location('agent_daemon', Path(__file__).with_name('agent_daemon.py'))
daemon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(daemon)


def msg(mid, kind='REQUEST', text='REQUEST: do it'):
    return dict(id=mid, seq=1, kind=kind, sender='pc-claude', text=text)


def matches(rule, command):
    """Claude Code Bash rule semantics: Bash(x:*) is a prefix match, Bash(x) an exact match."""
    if not (rule.startswith('Bash(') and rule.endswith(')')):
        return False
    body = rule[5:-1]
    return command.startswith(body[:-2]) if body.endswith(':*') else command == body


def allows(allowed, denied, command):
    """A deny wins over an allow; under dontAsk anything not allowed is refused."""
    return not any(matches(r, command) for r in denied) and any(matches(r, command) for r in allowed)


class SelectTest(unittest.TestCase):
    def test_new_request_starts_a_run(self):
        self.assertIs(daemon.select([msg('a')], {}, 6, time.time()), True)

    def test_acks_never_start_a_run(self):
        self.assertIs(daemon.select([msg('a', 'ACK', 'ACK #1')], {}, 6, time.time()), False)
        self.assertIs(daemon.select([msg('a', None, 'ACK #1 (protocol 3)')], {}, 6, time.time()), False)

    def test_a_seen_waiting_request_does_not_rerun(self):
        state = {'seen': {'a': 'x'}}
        self.assertIs(daemon.select([msg('a')], state, 6, time.time()), False)
        self.assertIs(daemon.select([msg('a'), msg('go', 'INFO', 'GO')], state, 6, time.time()), True)

    def test_three_failures_give_up(self):
        self.assertIs(daemon.select([msg('a')], {'failures': {'a': 2}}, 6, time.time()), True)
        self.assertIs(daemon.select([msg('a')], {'failures': {'a': 3}}, 6, time.time()), False)

    def test_rate_limit_counts_the_last_hour_only(self):
        now = time.time()
        self.assertEqual(daemon.select([msg('a')], {'runs': [now - 60] * 6}, 6, now), 'rate-limited')
        self.assertIs(daemon.select([msg('a')], {'runs': [now - 4000] * 6}, 6, now), True)


class PauseTest(unittest.TestCase):
    def test_pause_expires(self):
        with tempfile.TemporaryDirectory() as d:
            f = daemon.files(Path(d) / 'pi-claude.json')
            self.assertFalse(daemon.paused_until(f))
            daemon.write_json(f['pause'], {'until': time.time() + 60})
            self.assertTrue(daemon.paused_until(f))
            daemon.write_json(f['pause'], {'until': time.time() - 1})
            self.assertFalse(daemon.paused_until(f))


class PermissionTest(unittest.TestCase):
    def test_allowlist_is_closed(self):
        rules = daemon.allowed_tools(Path('/repo'), 'build/agent-mailbox/pi-claude.json')
        joined = ' '.join(rules)
        for forbidden in ('sudo', 'git push', 'git merge', './ci', 'cmake', 'make:', 'Agent', 'rm:',
                          'systemctl --user stop', 'systemctl --user start', 'kill'):
            self.assertNotIn(forbidden, joined)
        restarts = [r for r in rules if 'restart' in r]
        self.assertEqual(restarts, ['Bash(systemctl --user restart agent-mailbox.service)'])  # exact, no :*
        self.assertIn('Edit(//repo/.claude/worktrees/**)', rules)
        self.assertIn('Bash(python3 tools/agent_mailbox.py --config build/agent-mailbox/pi-claude.json:*)', rules)
        self.assertNotIn('Edit', [r for r in rules if r == 'Edit'])  # never unrestricted edits

    def test_denylist_protects_tokens_and_the_deployed_hmi(self):
        joined = ' '.join(daemon.DISALLOWED)
        self.assertIn('build/agent-mailbox/*.json', joined)
        self.assertIn('ksphmi/**', joined)
        self.assertIn('Bash(git push:*)', daemon.DISALLOWED)

    def test_only_the_mailbox_server_may_be_restarted(self):
        rules = daemon.allowed_tools(Path('/repo'), 'build/agent-mailbox/pi-claude.json')
        self.assertTrue(allows(rules, daemon.DISALLOWED, 'systemctl --user restart agent-mailbox.service'))
        self.assertTrue(allows(rules, daemon.DISALLOWED, 'journalctl _SYSTEMD_USER_UNIT=agent-mailbox.service -n 50'))
        for denied in ('systemctl --user restart ksphmi.service', 'systemctl --user restart ksphmi',
                       'systemctl --user restart agent-mailbox.service ksphmi.service',
                       'systemctl --user restart agent-daemon@pi-claude.service',
                       'systemctl --user stop agent-mailbox.service', 'systemctl --user start ksphmi.service',
                       'systemctl --user stop ksphmi.service'):
            self.assertFalse(allows(rules, daemon.DISALLOWED, denied), denied)
        self.assertIn('Bash(systemctl --user restart ksphmi:*)', daemon.DISALLOWED)
        self.assertIn('Bash(systemctl --user stop:*)', daemon.DISALLOWED)
        self.assertIn('Bash(systemctl --user start:*)', daemon.DISALLOWED)


if __name__ == '__main__':
    unittest.main()
