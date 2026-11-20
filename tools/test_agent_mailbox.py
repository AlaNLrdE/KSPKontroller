"""Exercise durability, authenticated transport, protocol 4 delivery state and protocol 3 interoperation."""
import contextlib
import io
import json
import socket
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import urllib.error
import urllib.parse
import urllib.request

import agent_mailbox
from agent_mailbox import (Journal, LockBusy, Conflict, MailboxError, make_server, request, digest, core_digest,
                           register, send_message, collect, pending_messages, mark_done, watch, history_messages,
                           LEASE, OVERDUE_ARCHIVE, OVERDUE_ACK)

S1, S2 = 'session-one-0000000001', 'session-two-0000000002'


class V3Client:
    """The protocol 3 client's collect logic (commit 979130f): events filtered by `to`, archive by digest."""
    def __init__(self, config):
        self.config, self.history = config, []

    def collect(self):
        after, events = 0, []
        while True:
            page = request(self.config, '/events?' + urllib.parse.urlencode({'after': after, 'wait': 0}))['events']
            events.extend(page)
            if len(page) < 100:
                break
            after = page[-1]['seq']
        for item in events:
            if item['to'] == self.config['peer']:
                self.history.append(item)
                request(self.config, '/archive', dict(seq=item['seq'], peer=self.config['peer'], digest=digest(item)))


class MailboxTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.journal = Journal(self.root / 'shared.jsonl')
        self.server = make_server('127.0.0.1', 0, self.journal, 'test-only-secret', self.root / 'evidence')
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.config = dict(url='http://127.0.0.1:%s' % self.server.server_port,
                           token='test-only-secret', peer='pc')  # a legacy protocol 3 identity
        self.quiet = contextlib.redirect_stdout(io.StringIO())
        self.quiet.__enter__()

    def tearDown(self):
        self.quiet.__exit__(None, None, None)
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.journal.close()
        self.temp.cleanup()

    def agent(self, identity, name=None):
        """A protocol 4 agent: its own config file (hence own session, history and done files)."""
        path = self.root / ((name or identity) + '.json')
        config = dict(self.config, peer=identity)
        path.write_text(json.dumps(config))
        register(config, path)
        return config, path

    def send(self, **changes):
        data = dict(sender='mac', to='pc', text='Ready to test: Δv', id='one')
        data.update(changes)
        return request(self.config, '/messages', data)

    def assertRefused(self, code, call, *args, **kwargs):
        with self.assertRaises((MailboxError, urllib.error.HTTPError)) as caught:
            call(*args, **kwargs)
        self.assertEqual(caught.exception.code, code)
        if isinstance(caught.exception, urllib.error.HTTPError):
            caught.exception.close()
        return caught.exception

    def reopen(self):
        restarted = Journal(self.journal.path)
        self.addCleanup(restarted.close)
        return restarted

    # -- registration --------------------------------------------------------------------------------------------
    def test_two_clients_on_one_identity_are_refused(self):
        first, first_path = self.agent('pc-codex')
        second, second_path = dict(self.config, peer='pc-codex'), self.root / 'other-pc-codex.json'
        second_path.write_text(json.dumps(second))
        error = self.assertRefused(409, register, second, second_path)
        self.assertIn('live session', str(error))
        self.assertRefused(409, register, second, second_path, takeover=True)  # never while the lease is alive
        self.assertRefused(409, collect, second, second_path)
        collect(first, first_path)  # the holder keeps working
        names = [(a['identity'], a['state'], a['tool']) for a in request(self.config, '/agents')['agents']]
        self.assertEqual(names, [('pc-codex', 'active', 'codex')])
        self.assertNotIn('session', json.dumps(request(self.config, '/agents')))
        # Journal level, with controlled time: lease expiry and explicit release allow a confirmed takeover
        j = self.journal
        j.register('mac-claude', S1, 'claude', '/a', now=1000.0)
        with self.assertRaises(Conflict):
            j.register('mac-claude', S2, 'claude', '/b', takeover=True, now=1000.0 + LEASE - 1)
        with self.assertRaises(Conflict):  # expired, but taking over must be explicit
            j.register('mac-claude', S2, 'claude', '/b', now=1000.0 + LEASE + 1)
        self.assertEqual(j.register('mac-claude', S2, 'claude', '/b', takeover=True, now=1100.0)['cwd'], '/b')
        with self.assertRaises(Conflict):  # the old session is now refused
            j.inbox('mac-claude', S1, now=1101.0)
        j.register('mac-claude', S2, 'claude', '/b', release=True, now=1102.0)
        self.assertEqual(j.register('mac-claude', S1, 'claude', '/a', takeover=True, now=1103.0)['state'], 'active')
        for bad in ('pc', 'pc_codex', 'laptop-claude'):
            with self.assertRaises(ValueError):
                j.register(bad, S1, 'x', '/')

    # -- register gate (mailbox #807) ------------------------------------------------------------------------------
    def gate(self, content):
        path = self.root / 'allowlist.json'
        if content is None:
            if path.exists():
                path.unlink()
        else:
            path.write_bytes(content if isinstance(content, bytes) else json.dumps(content).encode())

    def test_register_gate_is_off_without_an_allowlist(self):
        self.agent('pc-newcomer')  # current behaviour: any HOST-AGENT identity registers
        self.assertEqual(request(self.config, '/health')['register_gate'], 'off')
        self.assertEqual(request(self.config, '/agents')['register_gate'], 'off')

    def test_register_gate_refuses_unknown_and_admits_listed_identities(self):
        self.gate(['pc-claude', 'pi-claude'])
        stranger, stranger_path = dict(self.config, peer='pc-stranger'), self.root / 'pc-stranger.json'
        stranger_path.write_text(json.dumps(stranger))
        with contextlib.redirect_stderr(io.StringIO()) as log:
            error = self.assertRefused(403, register, stranger, stranger_path)
        self.assertIn(agent_mailbox.GATE_REFUSAL, str(error))
        self.assertIn('refused new identity pc-stranger', log.getvalue())  # the server log line
        self.assertNotIn('pc-stranger', self.journal.agents)
        self.assertFalse(stranger_path.with_suffix('.session.json').exists())
        self.agent('pc-claude')
        self.assertEqual(request(self.config, '/health')['register_gate'], 'on (2 identities)')
        self.gate(['pc-claude', 'pi-claude', 'pc-stranger'])  # re-read on each register: no restart
        register(stranger, stranger_path)
        self.assertEqual(request(self.config, '/agents')['register_gate'], 'on (3 identities)')

    def test_register_gate_never_locks_out_a_journal_identity(self):
        j = self.journal
        j.register('pc-claude', S1, 'claude', '/a', now=1000.0)
        j.register('mac-claude', S1, 'claude', '/m', now=1000.0)
        self.gate(['pc-claude'])  # mac-claude is live but was left off the list during the rollout
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(j.register('pc-claude', S1, 'claude', '/a', now=1010.0)['state'], 'active')  # renew
            self.assertEqual(j.register('pc-claude', S2, 'claude', '/b', takeover=True,
                                        now=1200.0)['cwd'], '/b')  # takeover
            self.assertEqual(j.register('mac-claude', S1, 'claude', '/m', now=1020.0)['state'], 'active')
            j.inbox('mac-claude', S1, now=1021.0)  # its lease renewal (heartbeat) keeps working
            self.assertEqual(j.register('mac-claude', S2, 'claude', '/n', takeover=True,
                                        now=1021.0 + LEASE + 1)['cwd'], '/n')
            with self.assertRaises(Conflict):  # the other gates (live lease) still apply
                j.register('pc-claude', S1, 'claude', '/a', takeover=True, now=1210.0)
            with self.assertRaises(agent_mailbox.Forbidden):
                j.register('mac-codex', S1, 'codex', '/x', now=1030.0)

    def test_invalid_allowlist_fails_closed_for_new_identities_only(self):
        self.journal.register('pc-claude', S1, 'claude', '/a', now=time.time())
        newcomer, newcomer_path = dict(self.config, peer='pc-newcomer'), self.root / 'pc-newcomer.json'
        newcomer_path.write_text(json.dumps(newcomer))
        for bad in (b'["pc-claude",', b'{"pc-claude": true}', ['pc-claude', 'pc'], ['pc-claude', 7], b'\xff\xfe'):
            self.gate(bad)
            with contextlib.redirect_stderr(io.StringIO()) as log:
                error = self.assertRefused(403, register, newcomer, newcomer_path)
                self.assertEqual(self.journal.register('pc-claude', S1, 'claude', '/a')['state'], 'active')
            self.assertIn('allowlist is invalid', str(error))
            self.assertIn('refused new identity pc-newcomer', log.getvalue())
            self.assertIn('invalid allowlist', request(self.config, '/health')['register_gate'])
        self.assertTrue(request(self.config, '/health')['ok'])  # the server never crashed
        self.gate(None)
        register(newcomer, newcomer_path)

    def test_register_gate_cli_shows_state_and_exits_non_zero_on_403(self):
        import subprocess
        import sys
        config, path = self.agent('pi-claude')
        self.gate(['pi-claude'])
        out = io.StringIO()
        with patch('sys.argv', ['agent_mailbox.py']), contextlib.redirect_stdout(out):
            agent_mailbox.main(['--config', str(path), 'agents'])
            agent_mailbox.main(['--config', str(path), 'status'])
        self.assertEqual(out.getvalue().count('MAILBOX register gate: on (1 identities)'), 2)
        stranger = self.root / 'pc-stranger.json'
        stranger.write_text(json.dumps(dict(self.config, peer='pc-stranger')))
        script = Path(agent_mailbox.__file__).resolve()
        with contextlib.redirect_stderr(io.StringIO()):
            run = subprocess.run([sys.executable, str(script), '--config', str(stranger), 'register'],
                                 capture_output=True, text=True, timeout=60, cwd=str(self.root))
        self.assertEqual(run.returncode, 1)
        self.assertIn('MAILBOX error: HTTP 403: ' + agent_mailbox.GATE_REFUSAL, run.stderr)

    def test_agents_on_one_host_have_separate_identities_and_inboxes(self):
        mac, mac_path = self.agent('mac-claude')
        claude, claude_path = self.agent('pc-claude')
        codex, codex_path = self.agent('pc-codex')
        send_message(mac, mac_path, 'pc-claude', 'REQUEST bridge', message_id='c1')
        send_message(mac, mac_path, 'pc-codex', 'REQUEST pi setup', message_id='x1')
        collect(claude, claude_path, acknowledge=False)
        self.assertEqual([m['text'] for m in pending_messages(claude, claude_path)], ['REQUEST bridge'])
        self.assertEqual([m['text'] for m in pending_messages(codex, codex_path)], ['REQUEST pi setup'])
        for bad in ('pc_codex', 'PC', 'laptop', 'pc-', 'pc-' + 'x' * 21):
            self.assertRefused(400, self.send, sender='mac', to=bad, id='bad-' + bad)

    # -- addressing ----------------------------------------------------------------------------------------------
    def test_broadcast_to_three_agents_is_read_by_each_separately(self):
        mac, mac_path = self.agent('mac-claude')
        others = [self.agent(name) for name in ('pc-claude', 'pc-codex', 'pi-claude')]
        sent = send_message(mac, mac_path, 'pc-claude,pc-codex,pi-claude', 'NOTICE: main is at 6f887e7', message_id='b1')
        self.assertEqual(sent['to'], ['pc-claude', 'pc-codex', 'pi-claude'])
        for index, (config, path) in enumerate(others):
            self.assertIn('b1', self.journal.messages)  # others' copies survive every earlier collect
            collect(config, path)
            self.assertEqual([m['id'] for m in history_messages(path.with_suffix('.history.md'))], ['b1'])
            states = request(self.config, '/track?id=b1')
            if index < 2:
                self.assertEqual(states['state'][config['peer']]['state'], 'archived')
        self.assertNotIn('b1', self.journal.messages)  # compacted once the last recipient archived it
        self.assertEqual(sorted(self.journal.receipts['b1']['recipients']), ['pc-claude', 'pc-codex', 'pi-claude'])

    def test_host_group_and_all_expand_to_registered_agents_at_send_time(self):
        mac, mac_path = self.agent('mac-claude')
        self.agent('pc-claude')
        self.agent('pc-codex')
        self.assertEqual(send_message(mac, mac_path, 'pc', 'INFO to PC', message_id='g1')['to'],
                         ['pc-claude', 'pc-codex'])
        self.agent('pi-claude')
        self.assertEqual(send_message(mac, mac_path, 'all', 'INFO to all', message_id='g2')['to'],
                         ['pc-claude', 'pc-codex', 'pi-claude'])  # sender excluded
        self.assertEqual(self.journal.messages['g1']['to'], ['pc-claude', 'pc-codex'])  # fixed when sent
        self.assertEqual(send_message(mac, mac_path, 'pc,pc-codex,pi', 'INFO dedup', message_id='g3')['to'],
                         ['pc-claude', 'pc-codex', 'pi-claude'])

    def test_unknown_recipient_and_empty_group_are_rejected(self):
        mac, mac_path = self.agent('mac-claude')
        self.agent('pc-claude')
        before = dict(self.journal.messages)
        self.assertIn('unknown recipient', str(self.assertRefused(400, send_message, mac, mac_path, 'pc-cluade', 'x')))
        self.assertRefused(400, send_message, mac, mac_path, 'pi', 'nobody registered on the Pi')
        self.assertRefused(400, send_message, mac, mac_path, 'pc-claude,laptop', 'one bad name refuses all')
        self.assertRefused(400, send_message, mac, mac_path, 'mac', 'only the sender on that host')
        self.assertEqual(self.journal.messages, before)

    def test_request_needs_exactly_one_owner_among_recipients(self):
        mac, mac_path = self.agent('mac-claude')
        claude, claude_path = self.agent('pc-claude')
        codex, codex_path = self.agent('pc-codex')
        self.assertRefused(400, send_message, mac, mac_path, 'pc', 'REQUEST: deploy')
        self.assertRefused(400, send_message, mac, mac_path, 'pc', 'REQUEST: deploy', owner='pi-claude')
        self.assertRefused(400, send_message, mac, mac_path, 'pc', 'INFO with owner', owner='pc-codex')
        self.assertEqual(send_message(mac, mac_path, 'pc-claude', 'REQUEST solo', message_id='solo')['owner'],
                         'pc-claude')  # a single recipient owns it by default
        m = send_message(mac, mac_path, 'pc', 'REQUEST: deploy', owner='pc-codex', message_id='r1')
        self.assertEqual((m['owner'], m['to']), ('pc-codex', ['pc-claude', 'pc-codex']))
        collect(claude, claude_path)
        collect(codex, codex_path)
        acks = [x for x in self.journal.messages.values() if x['kind'] == 'ACK']
        self.assertEqual([(a['sender'], a['to']) for a in acks], [('pc-claude', ['mac-claude']),
                                                                  ('pc-codex', ['mac-claude'])])
        self.assertEqual(acks[1]['text'].split()[1], '#%d' % m['seq'])  # only the owner ACKs r1
        self.assertIn('r1', self.journal.messages)  # informational copy archived, owner not done yet
        mark_done(codex, codex_path, message_id='r1')
        self.assertNotIn('r1', self.journal.messages)

    # -- per-recipient state -------------------------------------------------------------------------------------
    def test_collect_copies_before_archiving_and_requests_survive_until_done(self):
        mac, mac_path = self.agent('mac-claude')
        pc, pc_path = self.agent('pc-claude')
        first = send_message(mac, mac_path, 'pc-claude', 'REQUEST: investigate orbit', message_id='q1')
        history = collect(pc, pc_path)
        self.assertEqual([(m['id'], m['recipient']) for m in history_messages(history)], [('q1', 'pc-claude')])
        entry = self.journal.messages['q1']['state']['pc-claude']
        self.assertEqual(entry['state'], 'acked')
        self.assertLessEqual(entry['delivered'], entry['archived'])
        ack = self.journal.messages['ack-pc-claude-%d' % first['seq']]
        self.assertEqual((ack['to'], ack['kind']), (['mac-claude'], 'ACK'))
        collect(pc, pc_path)
        self.assertEqual(sum(m['kind'] == 'ACK' for m in self.journal.messages.values()), 1)  # no ACK multiplication
        self.assertEqual(len(history_messages(history)), 1)
        self.assertEqual([m['id'] for m in pending_messages(pc, pc_path)], ['q1'])
        mark_done(pc, pc_path, message_id='q1')
        self.assertEqual(pending_messages(pc, pc_path), [])
        self.assertEqual(self.journal.receipts['q1']['recipients'], {'pc-claude': 'done'})
        collect(mac, mac_path)  # the sender archives the ACK; ACKs never trigger ACKs
        self.assertEqual(self.journal.messages, {})

    def test_delivered_messages_stay_pending_until_done_in_any_order(self):
        mac, mac_path = self.agent('mac-claude')
        pc, pc_path = self.agent('pc-claude')
        send_message(mac, mac_path, 'pc-claude', 'First', message_id='one')
        send_message(mac, mac_path, 'pc-claude', 'Another request', message_id='two')
        watch(pc, pc_path, continuous=False)
        self.assertEqual(self.journal.messages['one']['state']['pc-claude']['state'], 'delivered')
        self.assertEqual([m['id'] for m in pending_messages(pc, pc_path)], ['one', 'two'])
        mark_done(pc, pc_path, seq=self.journal.messages['two']['seq'])  # per message, not "the oldest"
        self.assertEqual([m['id'] for m in pending_messages(pc, pc_path)], ['one'])
        with self.assertRaises(ValueError):
            mark_done(pc, pc_path, message_id='two')
        mark_done(pc, pc_path, message_id='one')
        self.assertEqual(pending_messages(pc, pc_path), [])

    def test_pending_ignores_outgoing_messages(self):
        mac, mac_path = self.agent('mac-claude')
        pc, pc_path = self.agent('pc-claude')
        session = json.loads(pc_path.with_suffix('.session.json').read_text())['session']
        for i in range(102):
            self.journal.send(dict(v=4, sender='pc-claude', session=session, to='mac-claude', text='Outgoing',
                                   id=str(i)))
        incoming = send_message(mac, mac_path, 'pc-claude', 'Incoming', message_id='incoming')
        self.assertEqual([m['seq'] for m in pending_messages(pc, pc_path)], [incoming['seq']])
        mark_done(pc, pc_path, message_id='incoming')
        self.assertEqual(pending_messages(pc, pc_path), [])

    def test_crash_between_archive_and_done_is_recovered_from_the_server(self):
        mac, mac_path = self.agent('mac-claude')
        pc, pc_path = self.agent('pc-claude')
        send_message(mac, mac_path, 'pc-claude', 'REQUEST: rebuild', message_id='crash')
        real = agent_mailbox.request
        def fail_state(config, route, data=None):
            if route == '/state':
                raise OSError('connection lost after the local history write')
            return real(config, route, data)
        with patch('agent_mailbox.request', side_effect=fail_state):
            with self.assertRaises(OSError):
                collect(pc, pc_path)
        self.assertEqual(self.journal.messages['crash']['state']['pc-claude']['state'], 'delivered')
        collect(pc, pc_path)  # the retry archives on the server without duplicating local history
        self.assertEqual(len(history_messages(pc_path.with_suffix('.history.md'))), 1)
        self.assertEqual(self.journal.messages['crash']['state']['pc-claude']['state'], 'acked')
        pc_path.with_suffix('.history.md').unlink()  # local files lost: the server still lists the work
        self.assertEqual([m['id'] for m in pending_messages(pc, pc_path)], ['crash'])
        mark_done(pc, pc_path, message_id='crash')
        pc_path.with_suffix('.done.json').unlink()
        collect(pc, pc_path)
        self.assertEqual(pending_messages(pc, pc_path), [])  # the done mark on the server survives
        self.assertEqual(self.journal.track('crash')['recipients'], {'pc-claude': 'done'})

    def test_archive_failure_leaves_server_state_and_wrong_recipient_cannot_archive(self):
        first = self.send(text='REQUEST: important')
        pc, pc_path = self.agent('pc-claude')
        request(self.config, '/messages', dict(sender='mac', to='pc-claude', text='REQUEST: two', id='two'))
        with patch('agent_mailbox.archive_message', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                collect(pc, pc_path)
        self.assertEqual(self.journal.messages['two']['state']['pc-claude']['state'], 'delivered')
        self.assertEqual(self.journal.events(0, 0), [first, dict(first, seq=2, id='two', to='pc-claude',
                                                                 text='REQUEST: two',
                                                                 time=self.journal.messages['two']['time'])])
        with self.assertRaises(ValueError):
            self.journal.remove(1, 'mac', digest(first))
        with self.assertRaises(ValueError):
            self.journal.remove(1, 'pc', 'wrong copied digest')

    def test_tracking_and_watcher_report_acked_and_done(self):
        mac, mac_path = self.agent('mac-claude')
        pc, pc_path = self.agent('pc-claude')
        send_message(mac, mac_path, 'pc-claude', 'REQUEST: run tests', message_id='t1')
        self.assertEqual(request(self.config, '/track?id=t1')['state']['pc-claude']['state'], 'queued')
        collect(pc, pc_path)
        with contextlib.redirect_stdout(io.StringIO()) as out:
            watch(mac, mac_path, continuous=False)
        self.assertIn('MAILBOX TRACK #1 t1: pc-claude acked', out.getvalue())
        self.assertIn('ACK #1', out.getvalue())  # the ACK itself is an incoming message for the sender
        mark_done(pc, pc_path, message_id='t1')
        with contextlib.redirect_stdout(io.StringIO()) as out:
            watch(mac, mac_path, continuous=False)
        self.assertIn('MAILBOX TRACK #1 t1: pc-claude done', out.getvalue())
        with contextlib.redirect_stdout(io.StringIO()) as out:
            watch(mac, mac_path, continuous=False)
        self.assertEqual(out.getvalue(), '')  # each change is reported once
        self.assertTrue(request(self.config, '/track?id=t1')['closed'])
        self.assertRefused(404, request, self.config, '/track?id=nothing')

    def test_lease_expiry_and_stale_items_are_overdue(self):
        j = self.journal
        j.register('mac-claude', S1, 'claude', '/m', now=1000.0)
        j.register('pc-claude', S2, 'claude', '/p', now=1000.0)
        j.send(dict(v=4, sender='mac-claude', session=S1, to='pc-claude', text='REQUEST: flash', id='o1',
                    deadline=1000.0 + 60), now=1000.0)
        self.assertEqual(j.overdue(now=1001.0), [])
        reasons = lambda t: sorted(i['reason'].split(' ')[0] for i in j.overdue(now=t) if i['id'] == 'o1')
        self.assertEqual(reasons(1000.0 + LEASE + 1), ['past', 'recipient'])  # deadline 60 s, lease 90 s
        j.inbox('pc-claude', S2, now=1001.0 + LEASE)  # heartbeat + delivered
        self.assertEqual(reasons(1002.0 + LEASE), ['past'])
        t = 1001.0 + max(OVERDUE_ACK, LEASE + OVERDUE_ARCHIVE) + 1
        j.inbox('pc-claude', S2, now=t - 1)  # alive, but neither archived nor acked
        self.assertEqual(reasons(t), ['REQUEST', 'delivered', 'past'])
        self.assertEqual(request(self.config, '/health')['protocol'], 4)

    def test_retire_drops_a_dead_identity_from_routing_and_keeps_history(self):
        j, now = self.journal, 1000.0
        j.register('pi-claude', S1, 'claude', '/p', now=now)
        j.register('pc-codex', S2, 'codex', '/c', now=now)
        j.register('mac-claude', 'session-mac-000000000003', 'claude', '/m', now=now)
        send = lambda text, mid, to='all', owner=None, t=now: j.send(  # noqa: E731
            dict(v=4, sender='pi-claude', session=S1, to=to, owner=owner, text=text, id=mid), now=t)
        send('REQUEST: owned by codex', 'r1', to='pc-codex')
        send('INFO: to everybody', 'i1')
        later = now + LEASE + 10
        with self.assertRaises(Conflict):  # a live lease is refused
            j.retire('pi-claude', S1, 'pc-codex', now=now + 1)
        with self.assertRaises(Conflict) as caught:  # an owned unfinished REQUEST must be reassigned first
            j.retire('pi-claude', S1, 'pc-codex', now=later)
        self.assertIn('r1', str(caught.exception))
        with self.assertRaises(ValueError):
            j.retire('pi-claude', S1, 'pi-claude', now=later)
        j.reassign('pi-claude', S1, 'r1', 'mac-claude', now=later)
        retired = j.retire('pi-claude', S1, 'pc-codex', now=later)
        self.assertEqual((retired['retired'], retired['retired_by'], retired['state']), (later, 'pi-claude', 'gone'))
        self.assertEqual(j.retire('pi-claude', S1, 'pc-codex', now=later + 1)['retired'], later)  # idempotent
        self.assertEqual([a['identity'] for a in j.agents_list(now=later)], ['mac-claude', 'pi-claude'])
        self.assertIn('pc-codex', [a['identity'] for a in j.agents_list(now=later, retired=True)])
        self.assertTrue(j.messages['i1']['state']['pc-codex']['retired'])  # no longer waits for codex
        self.assertNotIn('pc-codex', [o['recipient'] for o in j.overdue(now=later + 10 * OVERDUE_ARCHIVE)])
        self.assertNotIn('pc-codex', send('INFO: after', 'i2', t=later)['to'])
        with self.assertRaises(ValueError):
            send('INFO: direct', 'i3', to='pc-codex', t=later)
        with self.assertRaises(Conflict):  # its old session and a takeover are both refused
            j.inbox('pc-codex', S2, now=later)
        with self.assertRaises(Conflict):
            j.register('pc-codex', 'session-new-000000000004', 'codex', '/c', takeover=True, now=later)
        restarted = Journal(j.path)
        try:
            self.assertEqual(restarted.agents['pc-codex']['retired'], later)  # durable
        finally:
            restarted.close()  # before tearDown: Windows cannot delete an open journal
        self.assertIn('i1', j.messages)  # history kept for the other recipients
        j.retire('pi-claude', S1, 'pc-codex', undo=True, now=later + 2)
        self.assertEqual(j.register('pc-codex', 'session-new-000000000004', 'codex', '/c', takeover=True,
                                    now=later + 3)['state'], 'active')

    def test_retire_command_over_http(self):
        pi, pi_path = self.agent('pi-claude')
        self.journal.register('pc-codex', S2, 'codex', '/c', now=0.0)  # lease long expired
        with patch('sys.argv', ['agent_mailbox.py']):
            agent_mailbox.main(['--config', str(pi_path), 'retire', 'pc-codex'])
            agent_mailbox.main(['--config', str(pi_path), 'agents', '--retired'])
        self.assertEqual([a['identity'] for a in request(self.config, '/agents')['agents']], ['pi-claude'])
        self.assertEqual(len(request(self.config, '/agents?retired=1')['agents']), 2)
        self.assertRefused(404, agent_mailbox.agent_request, pi, pi_path, '/retire', dict(target='pc-nobody'))

    def test_reassignment_moves_the_owner_and_notifies_both(self):
        mac, mac_path = self.agent('mac-claude')
        claude, claude_path = self.agent('pc-claude')
        codex, codex_path = self.agent('pc-codex')
        send_message(mac, mac_path, 'pc-claude', 'REQUEST: HMI fix', message_id='h1')
        moved = agent_mailbox.agent_request(mac, mac_path, '/reassign', dict(id='h1', to='pc-codex'))
        self.assertEqual((moved['owner'], moved['to']), ('pc-codex', ['pc-claude', 'pc-codex']))
        self.assertTrue(moved['state']['pc-claude']['reassigned'])
        notice = self.journal.messages['reassign-1-1']
        self.assertEqual((notice['kind'], notice['to']), ('NOTICE', ['pc-claude', 'pc-codex']))
        collect(codex, codex_path)
        self.assertEqual(self.journal.messages['h1']['state']['pc-codex']['state'], 'acked')
        collect(claude, claude_path)
        self.assertNotIn('ack-pc-claude-1', self.journal.messages)  # the old owner no longer works on it
        self.assertEqual([m['id'] for m in pending_messages(claude, claude_path)], ['reassign-1-1'])
        mark_done(codex, codex_path, message_id='h1')
        self.assertEqual(self.journal.receipts['h1']['recipients'], {'pc-claude': 'reassigned', 'pc-codex': 'done'})
        self.assertRefused(404, agent_mailbox.agent_request, mac, mac_path, '/reassign', dict(id='h1', to='pc-claude'))
        send_message(mac, mac_path, 'pc-claude', 'INFO only', message_id='i1')
        self.assertRefused(400, agent_mailbox.agent_request, mac, mac_path, '/reassign', dict(id='i1', to='pc-codex'))

    def test_compaction_only_after_every_recipient_finished(self):
        j = self.journal
        for identity, session in (('mac-claude', S1), ('pc-claude', S2), ('pc-codex', 'session-three-000003')):
            j.register(identity, session, 'x', '/', now=1000.0)
        m = j.send(dict(v=4, sender='mac-claude', session=S1, to='pc', owner='pc-codex', text='REQUEST: merge',
                        id='c1'), now=1000.0)
        core = core_digest(m)
        j.set_state('pc-codex', 'session-three-000003', 'c1', 'done', core, now=1001.0)
        self.assertIn('c1', j.messages)  # the informational copy for pc-claude is not archived yet
        with self.assertRaises(ValueError):
            j.set_state('pc-claude', S2, 'c1', 'archived', 'wrong digest', now=1002.0)
        j.set_state('pc-claude', S2, 'c1', 'archived', core, now=1002.0)
        self.assertNotIn('c1', j.messages)
        self.assertEqual(j.receipts['c1']['recipients'], {'pc-claude': 'archived', 'pc-codex': 'done'})
        self.assertEqual(j.set_state('pc-claude', S2, 'c1', 'done', core, now=1003.0)['state'], 'closed')
        j.compact()
        self.assertNotIn(b'REQUEST: merge', j.path.read_bytes())  # only the receipt remains
        restarted = self.reopen()
        self.assertEqual((restarted.messages, restarted.last_seq), ({}, 1))
        self.assertEqual(restarted.receipts['c1']['core'], core)
        again = restarted.send(dict(v=4, sender='mac-claude', session=S1, to='pc', owner='pc-codex',
                                    text='REQUEST: merge', id='c1'), now=1004.0)
        self.assertTrue(again['archived'])  # idempotent retry after compaction, never resurrected

    # -- protocol 3 interoperation ---------------------------------------------------------------------------------
    def test_protocol3_client_interoperates_with_protocol4_server(self):
        self.assertEqual(request(self.config, '/health')['protocol'], 4)
        mac, mac_path = self.agent('mac-claude')
        claude, claude_path = self.agent('pc-claude')
        self.journal.register('pc-codex', S1, 'codex', '/', now=0.0)  # registered once, lease long expired
        old_pc, old_codex = V3Client(self.config), V3Client(dict(self.config, peer='pc-codex'))
        # A protocol 3 client sends to a protocol 4 agent; the agent collects it with protocol 4
        self.send(sender='mac', to='pc-claude', text='REQUEST: from an old client', id='v3a')
        collect(claude, claude_path)
        self.assertEqual(self.journal.messages['v3a']['state']['pc-claude']['state'], 'acked')
        self.assertEqual(self.journal.messages['ack-pc-claude-1']['to'], ['mac'])  # ACK to the literal sender
        # Old clients archive only their own copy of a broadcast
        send_message(mac, mac_path, 'pc-claude,pc-codex', 'NOTICE: both', message_id='both')
        send_message(mac, mac_path, 'pc', 'legacy inbox', message_id='legacy', legacy=True)
        old_codex.collect()
        old_pc.collect()
        self.assertEqual([m['id'] for m in old_codex.history], ['both'])
        self.assertEqual([m['id'] for m in old_pc.history], ['legacy'])
        self.assertEqual(self.journal.messages['both']['state']['pc-codex']['state'], 'archived')
        self.assertEqual(self.journal.messages['both']['state']['pc-claude']['state'], 'queued')
        self.assertNotIn('legacy', self.journal.messages)
        old_pc.collect()  # retry: nothing new, nothing duplicated
        self.assertEqual(len(old_pc.history), 1)
        # An old client cannot archive copies of an identity held by a live protocol 4 session
        self.send(sender='mac', to='pc-claude', text='INFO', id='v3b')
        self.assertRefused(409, V3Client(dict(self.config, peer='pc-claude')).collect)
        collect(claude, claude_path)
        self.assertEqual(sorted(m['id'] for m in history_messages(claude_path.with_suffix('.history.md'))),
                         ['both', 'v3a', 'v3b'])
        # Old locks still work
        self.assertTrue(request(self.config, '/locks', dict(action='acquire', name='pi-deploy', holder='pc',
                                                            ttl=600, reason='old client'))['acquired'])

    def test_legacy_host_inbox_and_history_are_drained_once(self):
        old = V3Client(self.config)
        self.send(text='REQUEST: left in the old pc inbox', id='l1')
        (self.root / 'pc.history.md').write_text('')  # old shared config's local history: one item unhandled
        agent_mailbox.archive_message(self.root / 'pc.history.md', dict(seq=9, id='l0', sender='mac', to='pc',
                                                                        text='REQUEST: archived earlier', time='t'))
        claude, claude_path = self.agent('pc-claude')
        collect(claude, claude_path, legacy_inbox=True)
        self.assertEqual([(m['id'], m['recipient']) for m in history_messages(claude_path.with_suffix('.history.md'))],
                         [('l0', 'pc'), ('l1', 'pc')])
        self.assertEqual(self.journal.messages['l1']['state']['pc']['state'], 'acked')
        old.collect()
        self.assertEqual(old.history, [])  # already archived for the pc identity
        self.assertEqual([m['id'] for m in pending_messages(claude, claude_path)], ['l1', 'l0'])
        mark_done(claude, claude_path, message_id='l1')
        mark_done(claude, claude_path, message_id='l0')
        self.assertEqual(pending_messages(claude, claude_path), [])
        self.assertNotIn('l1', self.journal.messages)

    def test_durable_deduplicated_send_and_conflicting_id(self):
        first = self.send()
        self.assertEqual(first, self.send())
        self.assertRefused(400, self.send, text='different contents')
        self.assertEqual(self.reopen().events(0, 0), [first])
        self.assertEqual(request(self.config, '/events?after=0')['events'], [first])

    def test_empty_queue_restart_keeps_monotonic_ids_and_deleted_send_dedup(self):
        first = self.send()
        self.journal.remove(first['seq'], 'pc', digest(first))
        restarted = self.reopen()
        self.assertEqual(restarted.messages, {})
        self.assertEqual(restarted.last_seq, 1)
        receipt = restarted.append(dict(sender='mac', to='pc', text='Ready to test: Δv', id='one'))
        self.assertTrue(receipt['archived'])
        self.assertEqual(restarted.messages, {})
        new = restarted.append(dict(sender='mac', to='pc', text='Next', id='next'))
        self.assertEqual(new['seq'], 2)
        self.assertEqual(restarted.events(1, 0), [new])

    def test_ack_and_result_are_archived_without_ack_loops(self):
        pc, pc_path = self.agent('pc-claude')
        self.send(to='pc-claude', text='ACK #12: working')
        self.send(to='pc-claude', id='two', text='RESULT #12: complete')
        history = collect(pc, pc_path)
        self.assertEqual(len(history_messages(history)), 2)
        self.assertEqual(self.journal.messages, {})
        self.assertEqual(request(self.config, '/health')['last_seq'], 2)

    # -- durability ------------------------------------------------------------------------------------------------
    def test_ten_concurrent_writers_keep_all_messages_in_order(self):
        mac, mac_path = self.agent('mac-claude')
        self.agent('pc-claude')
        errors = []
        def write(i):
            try:
                send_message(mac, mac_path, 'pc-claude', 'INFO %d' % i, message_id='w%d' % i)
            except Exception as error:  # noqa: BLE001 - reported below
                errors.append(error)
        threads = [threading.Thread(target=write, args=(i,)) for i in range(10)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(errors, [])
        restarted = self.reopen()
        self.assertEqual([m['seq'] for m in restarted.messages.values()], list(range(1, 11)))
        self.assertEqual(sorted(restarted.messages), sorted('w%d' % i for i in range(10)))

    def test_checksums_torn_final_record_recovered_and_corruption_refused(self):
        self.send()
        original = self.journal.path.read_bytes()
        self.journal.path.write_bytes(original + b'{"k":"msg","m":{"seq":2,')
        self.assertEqual(len(self.reopen().messages), 1)
        self.assertEqual(self.journal.path.read_bytes(), original)
        tampered = original.replace('Ready to test'.encode(), b'Reedy to test')
        self.assertNotEqual(tampered, original)
        self.journal.path.write_bytes(tampered)  # final record, complete line, wrong checksum
        with self.assertRaises(ValueError):
            Journal(self.journal.path)
        lines = original.splitlines(keepends=True)
        self.journal.path.write_bytes(lines[0] + b'not json\n' + b''.join(lines[1:]))
        with self.assertRaises(ValueError) as caught:
            Journal(self.journal.path)
        self.assertIn('record 2', str(caught.exception))
        self.journal.path.write_bytes(original)
        self.assertTrue(list((self.root / 'backup').glob('shared-????-??-??.jsonl')))  # daily backup copy

    def test_migrates_protocol3_and_older_journals(self):
        path = self.root / 'v3' / 'shared.jsonl'
        path.parent.mkdir()
        header = dict(mailbox_version=3, last_seq=7, locks={'pi-deploy': dict(
            name='pi-deploy', holder='mac-claude', reason='deploy', since=1.0, expires=4e9)},
            receipts={'gone': dict(seq=5, to='pc', digest='d5'), 'm6': dict(seq=6, to='mac', digest='d6')})
        m6 = dict(seq=6, id='m6', sender='pc', to='mac', text='REQUEST: still open', time='2026-11-16T10:00:00+00:00')
        m7 = dict(seq=7, id='m7', sender='mac', to='pc-codex', text='INFO', time='2026-11-16T10:01:00+00:00')
        text = ''.join(json.dumps(x) + '\n' for x in (header, m6, m7))
        path.write_text(text)
        j = Journal(path)
        self.addCleanup(j.close)
        self.assertEqual((j.last_seq, sorted(j.messages), sorted(j.receipts)), (7, ['m6', 'm7'], ['gone']))
        self.assertEqual((j.messages['m6']['owner'], j.messages['m6']['state']['mac']['state']), ('mac', 'queued'))
        self.assertEqual(j.locks['pi-deploy']['holder'], 'mac-claude')
        self.assertEqual(j.events(0, 0), [m6, m7])  # old clients still see the same events and digests
        self.assertEqual(j.remove(6, 'mac', digest(m6)), dict(removed=True, seq=6))
        self.assertEqual(j.remove(5, 'pc', 'any'), dict(removed=False, seq=5))  # removed under protocol 3
        backups = list((path.parent / 'backup').glob('shared.v3-*.jsonl'))
        self.assertEqual([b.read_text() for b in backups], [text])
        self.assertEqual(json.loads(path.read_bytes().split(b'\n', 1)[0])['mailbox_version'], 4)
        j.close()
        again = Journal(path)
        self.addCleanup(again.close)
        self.assertEqual(sorted(again.messages), ['m6', 'm7'])  # m6 stays until done by its owner
        self.assertEqual(again.messages['m6']['state']['mac']['state'], 'archived')
        headerless = self.root / 'v1.jsonl'
        headerless.write_text(json.dumps(m7) + '\n' + '{"seq": 8, "id"')  # oldest form with a torn tail
        oldest = Journal(headerless)
        self.addCleanup(oldest.close)
        self.assertEqual((sorted(oldest.messages), oldest.last_seq), (['m7'], 7))

    def test_shared_resource_locks_have_one_holder_and_expire(self):
        lock = lambda **d: request(self.config, '/locks', d)
        got = lock(action='acquire', name='pi-deploy', holder='mac-claude', ttl=600, reason='deploy 68ce1a7')
        self.assertTrue(got['acquired'])
        with self.assertRaises(urllib.error.HTTPError) as caught:
            lock(action='acquire', name='pi-deploy', holder='pc-claude', ttl=600, reason='deploy 6e37210')
        self.assertEqual(caught.exception.code, 409)
        self.assertEqual(json.loads(caught.exception.read())['lock']['holder'], 'mac-claude')
        caught.exception.close()
        self.assertRefused(409, lock, action='release', name='pi-deploy', holder='pc-codex')  # only the holder
        self.assertTrue(lock(action='acquire', name='pi-deploy', holder='mac-claude', ttl=900, reason='renew')['acquired'])
        self.assertEqual([l['name'] for l in request(self.config, '/locks')['locks']], ['pi-deploy'])
        self.assertEqual(self.reopen().locks['pi-deploy']['holder'], 'mac-claude')  # survives restart
        self.assertTrue(lock(action='release', name='pi-deploy', holder='mac-claude')['released'])
        self.assertEqual(request(self.config, '/locks')['locks'], [])
        # A crashed holder cannot block for ever: the lock expires by itself
        self.journal.lock('acquire', 'board-flash', 'pc-codex', 60, 'flash', now=1000.0)
        with self.assertRaises(LockBusy):
            self.journal.lock('acquire', 'board-flash', 'mac-claude', 60, 'flash', now=1030.0)
        self.assertTrue(self.journal.lock('acquire', 'board-flash', 'mac-claude', 60, 'flash', now=1061.0)['acquired'])
        for bad in (dict(name='Pi Deploy', ttl=600, reason='x'), dict(name='pi-deploy', ttl=5, reason='x'),
                    dict(name='pi-deploy', ttl=600, reason='')):
            with self.assertRaises(ValueError):
                self.journal.lock('acquire', bad['name'], 'mac', bad['ttl'], bad['reason'])

    # -- transport -------------------------------------------------------------------------------------------------
    def test_auth_and_no_file_or_config_exposure(self):
        self.assertRefused(401, request, dict(self.config, token='wrong'), '/events')
        self.assertRefused(404, request, self.config, '/../../config.json')
        with urllib.request.urlopen(self.config['url'] + '/client.py') as response:
            self.assertIn(b'class Journal:', response.read())

    def test_empty_or_invalid_state_file_reads_as_default_and_is_moved_aside(self):
        with tempfile.TemporaryDirectory() as d:
            for name, body in (('cursor.json', b''), ('track.json', b'{"half": ')):
                path = Path(d) / name
                path.write_bytes(body)
                with contextlib.redirect_stderr(io.StringIO()) as err:
                    self.assertEqual(agent_mailbox.read_json(path, {'through': 0}), {'through': 0})
                self.assertIn('unreadable state file', err.getvalue())
                self.assertFalse(path.exists())
                self.assertEqual(len(list(Path(d).glob(name + '.corrupt-*'))), 1)
                agent_mailbox.write_json(path, {'through': 7})
                self.assertEqual(agent_mailbox.read_json(path, {}), {'through': 7})
            self.assertEqual(list(Path(d).glob('*.tmp')), [])

    def test_state_writes_use_private_temps_and_retry_a_busy_target(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'cursor.json'
            self.assertNotEqual(agent_mailbox.private_temp(path), agent_mailbox.private_temp(path))
            real, calls = Path.replace, []

            def flaky(self_, target):
                calls.append(target)
                if len(calls) < 3:
                    raise PermissionError(32, 'in use')
                return real(self_, target)
            with patch.object(Path, 'replace', flaky), patch.object(agent_mailbox.time, 'sleep'):
                agent_mailbox.write_json(path, {'through': 3})
            self.assertEqual(len(calls), 3)
            self.assertEqual(agent_mailbox.read_json(path, {}), {'through': 3})
            with patch.object(Path, 'replace', side_effect=PermissionError(32, 'in use')), \
                    patch.object(agent_mailbox.time, 'sleep'):
                with self.assertRaises(PermissionError):
                    agent_mailbox.write_json(path, {'through': 4})
            self.assertEqual(list(Path(d).glob('*.tmp')), [])

    def test_long_poll_wakes_and_cursor_survives_restart(self):
        mac, mac_path = self.agent('mac-claude')
        pc, pc_path = self.agent('pc-claude')
        output = io.StringIO()
        def run():
            with contextlib.redirect_stdout(output):
                watch(pc, pc_path, once=True, continuous=True)
        worker = threading.Thread(target=run, daemon=True)
        worker.start()
        time.sleep(.1)
        send_message(mac, mac_path, 'pc-claude', 'Ready to test: Δv', message_id='lp')
        worker.join(timeout=5)
        self.assertFalse(worker.is_alive())
        self.assertIn('Ready to test: Δv', output.getvalue())
        self.assertEqual(json.loads(pc_path.with_suffix('.cursor.json').read_text())['seen'], ['lp'])
        self.assertEqual(len(pc_path.with_suffix('.inbox.jsonl').read_text().splitlines()), 1)
        with contextlib.redirect_stdout(io.StringIO()) as second:
            watch(pc, pc_path, continuous=False)
        self.assertEqual(second.getvalue(), '')

    def test_transient_dns_failure_is_retried_without_model_intervention(self):
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        calls = []
        def open_url(req, **kwargs):
            calls.append(req.full_url)
            if len(calls) == 1:
                raise urllib.error.URLError(socket.gaierror('temporary mDNS failure'))
            return opener.open(req, **kwargs)
        from types import SimpleNamespace
        fake = SimpleNamespace(open=open_url)
        with patch('agent_mailbox.urllib.request.build_opener', return_value=fake), \
                patch('agent_mailbox.time.sleep'):
            self.assertTrue(request(self.config, '/health')['ok'])
        self.assertEqual(len(calls), 2)

    def test_lost_send_reply_is_retried_without_duplicate_message(self):
        from types import SimpleNamespace
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        calls = []
        def open_url(req, **kwargs):
            calls.append(req.full_url)
            response = opener.open(req, **kwargs)
            if len(calls) == 1:
                response.close()  # server already stored the message, but the client lost its response
                raise urllib.error.URLError('reply lost')
            return response
        with patch('agent_mailbox.urllib.request.build_opener', return_value=SimpleNamespace(open=open_url)), \
                patch('agent_mailbox.time.sleep'):
            item = self.send()
        self.assertEqual(len(calls), 2)
        self.assertEqual(self.journal.events(0, 0), [item])

    def test_named_server_is_resolved_again_for_each_request(self):
        other_journal = Journal(self.root / 'other.jsonl')
        self.addCleanup(other_journal.close)
        other_journal.append(dict(sender='mac', to='pc', text='New endpoint', id='moved'))
        other = make_server('127.0.0.1', 0, other_journal, 'test-only-secret')
        worker = threading.Thread(target=other.serve_forever, daemon=True)
        worker.start()
        original = socket.getaddrinfo
        destinations, selected = [], [self.server.server_port]
        def resolve(host, port, *args, **kwargs):
            if host == 'mailbox-test.local':
                destinations.append(host)
                return original('127.0.0.1', selected[0], *args, **kwargs)
            return original(host, port, *args, **kwargs)
        config = dict(self.config, url='http://mailbox-test.local:50380')
        try:
            with patch('socket.getaddrinfo', side_effect=resolve):
                self.assertEqual(request(config, '/health')['messages'], 0)
                selected[0] = other.server_port  # DNS now points at a different live endpoint.
                self.assertEqual(request(config, '/health')['messages'], 1)
            self.assertEqual(destinations, ['mailbox-test.local'] * 2)
        finally:
            other.shutdown()
            other.server_close()
            worker.join()

    def test_watcher_reloads_endpoint_config_after_transport_failure(self):
        mac, mac_path = self.agent('mac-claude')
        pc, pc_path = self.agent('pc-claude')
        send_message(mac, mac_path, 'pc-claude', 'Moved server', message_id='reconnected')
        old = dict(pc, url='http://old-name.local:50380')
        pc_path.write_text(json.dumps(old))
        real, seen = agent_mailbox.request, []
        def exchange(config, route, data=None):
            seen.append(config['url'])
            if len(seen) == 1:
                pc_path.write_text(json.dumps(pc))
                raise OSError('previous address unavailable')
            return real(config, route, data)
        with patch('agent_mailbox.request', side_effect=exchange), patch('agent_mailbox.time.sleep'), \
                contextlib.redirect_stderr(io.StringIO()):
            watch(old, pc_path, once=True, continuous=True)
        self.assertEqual(seen, [old['url'], pc['url']])
        self.assertEqual(json.loads(pc_path.with_suffix('.cursor.json').read_text())['seen'], ['reconnected'])

    def test_named_evidence_download_rejects_traversal_and_credentials(self):
        folder = self.root / 'evidence'
        folder.mkdir()
        (folder / 'orbit.png').write_bytes(b'png-fixture')
        req = urllib.request.Request(self.config['url'] + '/artifact?name=orbit.png',
                                     headers={'Authorization': 'Bearer test-only-secret'})
        with urllib.request.urlopen(req) as response:
            self.assertEqual(response.read(), b'png-fixture')
        for name in ('../config.json', 'config.json', 'x%5Cconfig.txt'):
            self.assertRefused(400, request, self.config, '/artifact?name=' + name)


if __name__ == '__main__':
    unittest.main()
