"""Exercise durability, authenticated transport and background mailbox delivery."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

from agent_mailbox import Journal, make_server, receive, request


class MailboxTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.journal = Journal(self.root / 'shared.jsonl')
        self.server = make_server('127.0.0.1', 0, self.journal, 'test-only-secret')
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
