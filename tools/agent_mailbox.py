#!/usr/bin/env python3
"""Durable, authenticated LAN mailbox for human-authorized agent coordination (stdlib only)."""
import argparse
from datetime import datetime, timezone
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

PEERS = ('mac', 'pc')
MAX_BODY = 16384


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.tmp')
    with temp.open('w', encoding='utf-8') as f:
        os.chmod(temp, 0o600)
        json.dump(value, f, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    temp.replace(path)


class Journal:
    def __init__(self, path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.condition = threading.Condition()
        self.messages = []
        self.ids = {}
        # Recover a partial final write after a power loss; never silently discard middle corruption.
        if self.path.exists():
            raw = self.path.read_bytes()
            good = 0
            for line in raw.splitlines(keepends=True):
                try:
                    item = json.loads(line)
                    if item['seq'] != len(self.messages) + 1:
                        raise ValueError('non-contiguous sequence')
                except (ValueError, KeyError):
                    if good + len(line) != len(raw) or line.endswith(b'\n'):
                        raise ValueError('corrupt mailbox journal')
                    break
                self.messages.append(item)
                self.ids[item['id']] = item
                good += len(line)
            if good != len(raw):
                with self.path.open('r+b') as f:
                    f.truncate(good)
            elif raw and not raw.endswith(b'\n'):
                with self.path.open('ab') as f:
                    f.write(b'\n')

    def append(self, data):
        sender, recipient = data.get('sender'), data.get('to')
        body, message_id = data.get('text'), data.get('id')
        if (sender not in PEERS or recipient not in PEERS or sender == recipient
                or not isinstance(body, str) or not body.strip() or len(body) > 8000
                or not isinstance(message_id, str) or len(message_id) > 80 or not message_id):
            raise ValueError('sender/to must be different mac/pc peers; text 1..8000 characters; id required')
        with self.condition:
            if message_id in self.ids:
                old = self.ids[message_id]
                if any(old[key] != data[key] for key in ('sender', 'to', 'text')):
                    raise ValueError('message id already used with different contents')
                return old
            item = dict(seq=len(self.messages) + 1, id=message_id, sender=sender, to=recipient,
                        text=body, time=datetime.now(timezone.utc).isoformat())
            with self.path.open('ab') as f:
                os.chmod(self.path, 0o600)
                f.write((json.dumps(item, ensure_ascii=False) + '\n').encode('utf-8'))
                f.flush()
                os.fsync(f.fileno())
            self.messages.append(item)
            self.ids[message_id] = item
            self.condition.notify_all()
            return item

    def events(self, after, wait):
        deadline = time.monotonic() + wait
        with self.condition:
            while len(self.messages) <= after and time.monotonic() < deadline:
                self.condition.wait(deadline - time.monotonic())
            return self.messages[after:after + 100]


def make_server(host, port, journal, token):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass  # No credentials, message contents or idle poll noise in server logs.

        def reply(self, status, data, content_type='application/json'):
            payload = data if isinstance(data, bytes) else json.dumps(data).encode()
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(payload)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def authorized(self):
            if hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + token):
                return True
            self.reply(401, {'error': 'unauthorized'})
            return False

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            # Bootstrap only this public script. Never expose directories, config or journal files.
            if url.path == '/client.py':
                return self.reply(200, Path(__file__).read_bytes(), 'text/x-python; charset=utf-8')
            if not self.authorized():
                return
            if url.path == '/health':
                return self.reply(200, {'ok': True, 'messages': len(journal.messages)})
            if url.path != '/events':
                return self.reply(404, {'error': 'not found'})
            try:
                args = urllib.parse.parse_qs(url.query)
                after = int(args.get('after', ['0'])[0])
                wait = int(args.get('wait', ['0'])[0])
                if not 0 <= after <= len(journal.messages) or not 0 <= wait <= 25:
                    raise ValueError()
            except ValueError:
                return self.reply(400, {'error': 'invalid cursor or wait'})
            self.reply(200, {'events': journal.events(after, wait)})

        def do_POST(self):
            if not self.authorized():
                return
            if self.path != '/messages':
                return self.reply(404, {'error': 'not found'})
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= MAX_BODY:
                    raise ValueError('message too large or empty')
                data = json.loads(self.rfile.read(length))
                if not isinstance(data, dict):
                    raise ValueError('expected object')
                result = journal.append(data)
            except (ValueError, TypeError) as e:
                return self.reply(400, {'error': str(e)})
            self.reply(200, result)

    class Server(ThreadingHTTPServer):
        daemon_threads = True
        allow_reuse_address = True

        def finish_request(self, request, client_address):
            request.settimeout(35)
            super().finish_request(request, client_address)

    return Server((host, port), Handler)


def request(config, route, data=None):
    payload = None if data is None else json.dumps(data).encode()
    req = urllib.request.Request(config['url'].rstrip('/') + route, data=payload,
                                 headers={'Authorization': 'Bearer ' + config['token'],
                                          'Content-Type': 'application/json'})
    # LAN endpoints must not be redirected through an environment-configured HTTP proxy.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req, timeout=35) as response:
        return json.load(response)


def receive(config, cursor_path, once=False, continuous=False):
    cursor_path = Path(cursor_path)
    cursor = json.loads(cursor_path.read_text())['after'] if cursor_path.exists() else 0
    inbox = cursor_path.with_suffix('.inbox.jsonl')
    outage = False
    while True:
        try:
            events = request(config, '/events?' + urllib.parse.urlencode(
                {'after': cursor, 'wait': 25 if continuous else 0}))['events']
            if outage:
                print('MAILBOX reconnected', file=sys.stderr, flush=True)
            outage = False
        except (OSError, ValueError) as e:
            if not continuous or isinstance(e, urllib.error.HTTPError) and e.code < 500:
                raise
            if not outage:
                print('MAILBOX offline; retrying: ' + str(e), file=sys.stderr, flush=True)
            outage = True
            time.sleep(3)
            continue
        found = False
        for item in events:
            if item['to'] == config['peer']:
                inbox.parent.mkdir(parents=True, exist_ok=True)
                with inbox.open('a', encoding='utf-8') as f:
                    f.write(json.dumps(item, ensure_ascii=False) + '\n')
                    f.flush()
                    os.fsync(f.fileno())
                print('\aMAILBOX #{seq} {sender} -> {to} [{time}]\n{text}'.format(**item), flush=True)
                found = True
            cursor = item['seq']
            write_json(cursor_path, {'after': cursor})
        if not continuous or once and found:
            return


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', default='build/agent-mailbox/config.json')
    sub = parser.add_subparsers(dest='command', required=True)
    init = sub.add_parser('init')
    init.add_argument('--url', required=True)
    init.add_argument('--peer', choices=PEERS, required=True)
    init.add_argument('--token')
    serve = sub.add_parser('serve')
    serve.add_argument('--bind', default='127.0.0.1')
    serve.add_argument('--port', type=int, default=50380)
    serve.add_argument('--journal', default='build/agent-mailbox/shared.jsonl')
    send = sub.add_parser('send')
    send.add_argument('--to', choices=PEERS, required=True)
    content = send.add_mutually_exclusive_group(required=True)
    content.add_argument('--text')
    content.add_argument('--file', type=Path)
    send.add_argument('--id', default=None, help='Reuse this id when retrying an uncertain send')
    sub.add_parser('status')
    sub.add_parser('inbox')
    watch = sub.add_parser('watch')
    watch.add_argument('--once', action='store_true', help='Exit when an incoming message arrives; re-arm after handling it')
    args = parser.parse_args(argv)
    config_path = Path(args.config)
    if args.command == 'init':
        if config_path.exists():
            parser.error('configuration already exists; use another path instead of rotating a live token')
        parsed = urllib.parse.urlsplit(args.url)
        if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.query or parsed.fragment:
            parser.error('expected an HTTP(S) server URL without credentials or query')
        write_json(config_path, {'url': args.url.rstrip('/'), 'peer': args.peer,
                                 'token': args.token or secrets.token_urlsafe(32)})
        print('Created ' + str(config_path) + ' (private; do not commit)')
        return
    config = json.loads(config_path.read_text(encoding='utf-8'))
    if args.command == 'serve':
        server = make_server(args.bind, args.port, Journal(args.journal), config['token'])
        print('MAILBOX listening on %s:%s; journal %s' % (*server.server_address, args.journal), flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()
    elif args.command == 'send':
        body = args.text if args.text is not None else args.file.read_text(encoding='utf-8')
        item = request(config, '/messages', {'id': args.id or str(uuid.uuid4()), 'sender': config['peer'],
                                           'to': args.to, 'text': body})
        print('Stored message #{seq} ({id}); receipt is not an acknowledgement from the other agent'.format(**item))
    elif args.command == 'status':
        print(json.dumps(request(config, '/health')))
    else:
        receive(config, config_path.with_suffix('.cursor.json'), once=getattr(args, 'once', False),
                continuous=args.command == 'watch')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError) as error:
        print('MAILBOX error: ' + str(error), file=sys.stderr)
        sys.exit(1)
