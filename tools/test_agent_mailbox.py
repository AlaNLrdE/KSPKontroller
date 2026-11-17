"""Exercise durability, authenticated transport and background mailbox delivery."""
import contextlib
import io
import json
import socket
from pathlib import Path
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

from agent_mailbox import Journal, LockBusy, make_server, receive, request, pending_messages, mark_handled, collect, history_messages, digest


class MailboxTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.journal = Journal(self.root / 'shared.jsonl')
        self.server = make_server('127.0.0.1', 0, self.journal, 'test-only-secret', self.root / 'evidence')
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.config = dict(url='http://127.0.0.1:%s' % self.server.server_port,
                           token='test-only-secret', peer='pc')

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def send(self, **changes):
        data = dict(sender='mac', to='pc', text='Ready to test: Δv', id='one')
        data.update(changes)
        return request(self.config, '/messages', data)

    def test_agents_on_one_host_have_separate_identities_and_inboxes(self):
        # Two agents on the PC (Claude Code and Codex) must never share a config: each one collects only its own.
        self.send(sender='mac', to='pc-claude', text='REQUEST bridge', id='c1')
        self.send(sender='mac', to='pc-codex', text='REQUEST pi setup', id='x1')
        claude, codex = dict(self.config, peer='pc-claude'), dict(self.config, peer='pc-codex')
        with contextlib.redirect_stdout(io.StringIO()):
            collect(claude, self.root / 'pc-claude.json', acknowledge=False)
        self.assertEqual([m['text'] for m in pending_messages(claude, self.root / 'pc-claude.handled.json')],
                         ['REQUEST bridge'])
        self.assertEqual([m['text'] for m in pending_messages(codex, self.root / 'pc-codex.handled.json')],
                         ['REQUEST pi setup'])  # still on the server: Claude's collect did not take it
        for bad in ('pc_codex', 'PC', 'laptop', 'pc-', 'pc-' + 'x' * 21):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.send(sender='mac', to=bad, id='bad-' + bad)
            self.assertEqual(caught.exception.code, 400)
            caught.exception.close()

    def test_shared_resource_locks_have_one_holder_and_expire(self):
        lock = lambda **d: request(self.config, '/locks', d)
        got = lock(action='acquire', name='pi-deploy', holder='mac-claude', ttl=600, reason='deploy 68ce1a7')
        self.assertTrue(got['acquired'])
        with self.assertRaises(urllib.error.HTTPError) as caught:
            lock(action='acquire', name='pi-deploy', holder='pc-claude', ttl=600, reason='deploy 6e37210')
        self.assertEqual(caught.exception.code, 409)
        self.assertEqual(json.loads(caught.exception.read())['lock']['holder'], 'mac-claude')
        caught.exception.close()
        with self.assertRaises(urllib.error.HTTPError) as caught:  # only the holder releases it
            lock(action='release', name='pi-deploy', holder='pc-codex')
        self.assertEqual(caught.exception.code, 409)
        caught.exception.close()
        self.assertTrue(lock(action='acquire', name='pi-deploy', holder='mac-claude', ttl=900, reason='renew')['acquired'])
        self.assertEqual([l['name'] for l in request(self.config, '/locks')['locks']], ['pi-deploy'])
        self.assertEqual(Journal(self.journal.path).locks['pi-deploy']['holder'], 'mac-claude')  # survives restart
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

    def test_auth_and_no_file_or_config_exposure(self):
        with self.assertRaises(urllib.error.HTTPError) as caught:
            request(dict(self.config, token='wrong'), '/events')
        self.assertEqual(caught.exception.code, 401)
        caught.exception.close()
        with self.assertRaises(urllib.error.HTTPError) as caught:
            request(self.config, '/../../config.json')
        self.assertEqual(caught.exception.code, 404)
        caught.exception.close()
        with urllib.request.urlopen(self.config['url'] + '/client.py') as response:
            self.assertIn(b'class Journal:', response.read())

    def test_durable_deduplicated_send_and_conflicting_id(self):
        first = self.send()
        self.assertEqual(first, self.send())
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.send(text='different contents')
        self.assertEqual(caught.exception.code, 400)
        caught.exception.close()
        restarted = Journal(self.journal.path)
        self.assertEqual(restarted.messages, [first])
        self.assertEqual(request(self.config, '/events?after=0')['events'], [first])

    def test_long_poll_wakes_and_cursor_survives_restart(self):
        cursor = self.root / 'pc.cursor.json'
        output = io.StringIO()
        def watch():
            with contextlib.redirect_stdout(output):
                receive(self.config, cursor, once=True, continuous=True)
        worker = threading.Thread(target=watch, daemon=True)
        worker.start()
        time.sleep(.05)
        self.send()
        worker.join(timeout=3)
        self.assertFalse(worker.is_alive())
        self.assertIn('Ready to test: Δv', output.getvalue())
        self.assertEqual(json.loads(cursor.read_text())['after'], 1)
        self.assertEqual(len(cursor.with_suffix('.inbox.jsonl').read_text().splitlines()), 1)
        with contextlib.redirect_stdout(io.StringIO()) as second:
            receive(self.config, cursor)
        self.assertEqual(second.getvalue(), '')

    def test_concurrent_writers_keep_all_messages_in_order(self):
        threads = [threading.Thread(target=self.send, kwargs={'id': str(i)}) for i in range(12)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(len(self.journal.messages), 12)
        self.assertEqual([item['seq'] for item in Journal(self.journal.path).messages], list(range(1, 13)))

    def test_delivered_messages_stay_pending_until_explicitly_handled(self):
        first = self.send()
        self.send(id='second', text='Another request')
        handled = self.root / 'pc.handled.json'
        with contextlib.redirect_stdout(io.StringIO()):
            receive(self.config, self.root / 'pc.cursor.json')
        self.assertEqual([m['seq'] for m in pending_messages(self.config, handled)], [1, 2])
        with self.assertRaises(ValueError):
            mark_handled(self.config, handled, 2)
        mark_handled(self.config, handled, first['seq'])
        self.assertEqual([m['seq'] for m in pending_messages(self.config, handled)], [2])
        mark_handled(self.config, handled, 2)
        self.assertEqual(pending_messages(self.config, handled), [])

    def test_pending_pages_over_outgoing_messages_without_touching_delivery_state(self):
        for i in range(102):
            self.journal.append(dict(sender='pc', to='mac', text='Outgoing', id=str(i)))
        self.send(id='incoming')
        handled = self.root / 'pc.handled.json'
        self.assertEqual([m['seq'] for m in pending_messages(self.config, handled)], [103])
        self.assertFalse(handled.exists())
        mark_handled(self.config, handled, 103)
        self.assertEqual(pending_messages(self.config, handled), [])

    def test_collect_copies_before_removing_and_requests_survive_in_history(self):
        first = self.send(text='REQUEST: investigate orbit')
        config_path = self.root / 'pc.json'
        history = collect(self.config, config_path)
        self.assertEqual(history_messages(history), [first])
        self.assertNotIn(first, self.journal.messages)
        self.assertEqual([m['seq'] for m in pending_messages(self.config, self.root / 'pc.handled.json')], [1])
        ack = self.journal.messages[0]
        self.assertTrue(ack['text'].startswith('ACK #1:'))
        self.assertEqual(ack['to'], 'mac')
        collect(self.config, config_path)
        self.assertEqual(len(self.journal.messages), 1)  # retry does not multiply ACKs
        mark_handled(self.config, self.root / 'pc.handled.json', 1)
        self.assertEqual(pending_messages(self.config, self.root / 'pc.handled.json'), [])

    def test_archive_failure_leaves_shared_message_and_wrong_recipient_cannot_delete(self):
        first = self.send(text='REQUEST: important')
        from unittest.mock import patch
        with patch('agent_mailbox.archive_message', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                collect(self.config, self.root / 'pc.json')
        self.assertEqual(self.journal.messages, [first])
        with self.assertRaises(ValueError):
            self.journal.remove(1, 'mac', digest(first))
        with self.assertRaises(ValueError):
            self.journal.remove(1, 'pc', 'wrong copied digest')

    def test_empty_queue_restart_keeps_monotonic_ids_and_deleted_send_dedup(self):
        first = self.send()
        self.journal.remove(first['seq'], 'pc', digest(first))
        restarted = Journal(self.journal.path)
        self.assertEqual(restarted.messages, [])
        self.assertEqual(restarted.last_seq, 1)
        receipt = restarted.append(dict(sender='mac', to='pc', text='Ready to test: Δv', id='one'))
        self.assertTrue(receipt['archived'])
        self.assertEqual(restarted.messages, [])
        new = restarted.append(dict(sender='mac', to='pc', text='Next', id='next'))
        self.assertEqual(new['seq'], 2)
        self.assertEqual(restarted.events(1, 0), [new])

    def test_ack_and_result_are_archived_without_ack_loops(self):
        self.send(text='ACK #12: working')
        self.send(id='two', text='RESULT #12: complete')
        history = collect(self.config, self.root / 'pc.json')
        self.assertEqual(len(history_messages(history)), 2)
        self.assertEqual(self.journal.messages, [])
        self.assertEqual(request(self.config, '/health')['last_seq'], 2)

    def test_transient_dns_failure_is_retried_without_model_intervention(self):
        from unittest.mock import patch
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
        from unittest.mock import patch
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
        self.assertEqual(self.journal.messages, [item])

    def test_named_server_is_resolved_again_for_each_request(self):
        from unittest.mock import patch
        other_journal = Journal(self.root / 'other.jsonl')
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
        from unittest.mock import patch
        config_path = self.root / 'pc.json'
        old = dict(self.config, url='http://old-name.local:50380')
        config_path.write_text(json.dumps(old))
        seen = []
        event = dict(seq=1, id='reconnected', sender='mac', to='pc', text='Moved server', time='now')
        def exchange(config, route):
            seen.append(config['url'])
            if len(seen) == 1:
                config_path.write_text(json.dumps(self.config))
                raise OSError('previous address unavailable')
            return {'events': [event]}
        with patch('agent_mailbox.request', side_effect=exchange), patch('agent_mailbox.time.sleep'), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            receive(old, self.root / 'pc.cursor.json', once=True, continuous=True, config_path=config_path)
        self.assertEqual(seen, [old['url'], self.config['url']])
        self.assertEqual(json.loads((self.root / 'pc.cursor.json').read_text())['after'], 1)

    def test_named_evidence_download_rejects_traversal_and_credentials(self):
        folder = self.root / 'evidence'
        folder.mkdir()
        (folder / 'orbit.png').write_bytes(b'png-fixture')
        req = urllib.request.Request(self.config['url'] + '/artifact?name=orbit.png',
                                     headers={'Authorization': 'Bearer test-only-secret'})
        with urllib.request.urlopen(req) as response:
            self.assertEqual(response.read(), b'png-fixture')
        for name in ('../config.json', 'config.json', 'x%5Cconfig.txt'):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                request(self.config, '/artifact?name=' + name)
            self.assertEqual(caught.exception.code, 400)
            caught.exception.close()

    def test_partial_final_write_recovered_but_middle_corruption_rejected(self):
        self.send()
        original = self.journal.path.read_bytes()
        self.journal.path.write_bytes(original + b'{"seq":2,')
        self.assertEqual(len(Journal(self.journal.path).messages), 1)
        self.assertEqual(self.journal.path.read_bytes(), original)
        self.journal.path.write_bytes(b'bad line\n' + original)
        with self.assertRaises(ValueError):
            Journal(self.journal.path)


if __name__ == '__main__':
    unittest.main()
