#!/usr/bin/env python3
"""Durable, authenticated LAN mailbox for human-authorized agent coordination (stdlib only)."""
import argparse
import base64
import hashlib
from datetime import datetime, timezone
import hmac
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
from pathlib import Path
import secrets
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

PEERS = ('mac', 'pc')  # legacy host identities; protocol 3 adds one identity per agent
MAX_BODY = 16384
PROTOCOL = 3
# An identity is a host, optionally with the agent on it: mac, pc, pi, pc-claude, pc-codex, mac-claude ...
# Every agent uses its own identity and config (own cursor, history and handled files); two agents never share one.
IDENTITY = re.compile(r'^(mac|pc|pi)(-[a-z0-9]{1,20})?$')
LOCK_NAME = re.compile(r'^[a-z0-9][a-z0-9._-]{0,39}$')
LOCK_TTL = (60, 14400)


def valid_identity(name):
    return isinstance(name, str) and bool(IDENTITY.match(name))


class LockBusy(ValueError):
    """A shared resource lock is held by another agent."""
    def __init__(self, lock):
        super().__init__('lock %s is held by %s until %s (%s)' % (
            lock['name'], lock['holder'], datetime.fromtimestamp(lock['expires'], timezone.utc).isoformat(),
            lock['reason']))
        self.lock = lock


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


def digest(data):
    return hashlib.sha256(json.dumps(data, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def replace_text(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.tmp')
    with temp.open('w', encoding='utf-8') as f:
        os.chmod(temp, 0o600)
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    temp.replace(path)


class Journal:
    """Active queue plus compact receipts, atomically replaced; accepts legacy append-only journals."""
    def __init__(self, path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.condition = threading.Condition()
        self.messages, self.ids, self.receipts = [], {}, {}
        self.locks = {}  # name -> {name, holder, reason, since, expires}: shared resources (protocol 3)
        self.last_seq = 0
        if self.path.exists():
            raw = self.path.read_bytes()
            good, previous = 0, 0
            for line in raw.splitlines(keepends=True):
                try:
                    item = json.loads(line)
                    if not good and item.get('mailbox_version') in (2, 3):
                        self.last_seq, self.receipts = item['last_seq'], item['receipts']
                        self.locks = item.get('locks', {})
                    else:
                        if item['seq'] <= previous:
                            raise ValueError('non-increasing sequence')
                        previous = item['seq']
                        self.messages.append(item)
                        self.ids[item['id']] = item
                        self.last_seq = max(self.last_seq, item['seq'])
                        self.receipts[item['id']] = self.receipt(item)
                except (ValueError, KeyError):
                    if good + len(line) != len(raw) or line.endswith(b'\n'):
                        raise ValueError('corrupt mailbox journal')
                    break
                good += len(line)
            if good != len(raw):
                with self.path.open('r+b') as f:
                    f.truncate(good)
            elif raw and not raw.endswith(b'\n'):
                with self.path.open('ab') as f:
                    f.write(b'\n')

    @staticmethod
    def receipt(item):
        return dict(seq=item['seq'], to=item['to'],
                    digest=digest({key: item[key] for key in ('sender', 'to', 'text')}))

    def persist(self):
        header = dict(mailbox_version=PROTOCOL, last_seq=self.last_seq, receipts=self.receipts, locks=self.locks)
        replace_text(self.path, ''.join(json.dumps(item, ensure_ascii=False) + '\n'
                                       for item in [header] + self.messages))

    def append(self, data):
        sender, recipient = data.get('sender'), data.get('to')
        body, message_id = data.get('text'), data.get('id')
        if (not valid_identity(sender) or not valid_identity(recipient) or sender == recipient
                or not isinstance(body, str) or not body.strip() or len(body) > 8000
                or not isinstance(message_id, str) or len(message_id) > 80 or not message_id):
            raise ValueError('sender/to must be different identities (mac, pc, pi or HOST-AGENT such as pc-codex); '
                             'text 1..8000 characters; id required')
        with self.condition:
            if message_id in self.receipts:
                old = self.receipts[message_id]
                if old['digest'] != digest({key: data[key] for key in ('sender', 'to', 'text')}):
                    raise ValueError('message id already used with different contents')
                return self.ids.get(message_id, dict(seq=old['seq'], id=message_id, archived=True))
            item = dict(seq=self.last_seq + 1, id=message_id, sender=sender, to=recipient,
                        text=body, time=datetime.now(timezone.utc).isoformat())
            self.last_seq = item['seq']
            self.messages.append(item)
            self.ids[message_id] = item
            self.receipts[message_id] = self.receipt(item)
            try:
                self.persist()
            except OSError:
                self.messages.pop()
                del self.ids[message_id], self.receipts[message_id]
                self.last_seq -= 1
                raise
            self.condition.notify_all()
            return item

    def remove(self, seq, peer, copied_digest):
        with self.condition:
            item = next((m for m in self.messages if m['seq'] == seq), None)
            if item is None:
                if any(r['seq'] == seq and r['to'] == peer for r in self.receipts.values()):
                    return dict(removed=False, seq=seq)
                raise ValueError('unknown message or wrong recipient')
            if peer != item['to'] or copied_digest != digest(item):
                raise ValueError('recipient or archived copy does not match')
            old = self.messages
            self.messages = [m for m in old if m['seq'] != seq]
            try:
                self.persist()
            except OSError:
                self.messages = old
                raise
            del self.ids[item['id']]
            return dict(removed=True, seq=seq)

    def _prune(self, now):
        expired = [name for name, lock in self.locks.items() if lock['expires'] <= now]
        for name in expired:
            del self.locks[name]
        return bool(expired)

    def lock(self, action, name, holder, ttl=None, reason='', now=None):
        """Acquire (or renew, by the same holder), release or list shared-resource locks. A lock held by another
        agent raises LockBusy; it expires by itself after its TTL, so a crashed agent cannot block for ever."""
        now = time.time() if now is None else now
        with self.condition:
            changed = self._prune(now)
            if action == 'list':
                if changed:
                    self.persist()
                return sorted(self.locks.values(), key=lambda l: l['name'])
            if not isinstance(name, str) or not LOCK_NAME.match(name) or not valid_identity(holder):
                raise ValueError('lock name [a-z0-9._-] up to 40 characters and a valid holder identity required')
            old = dict(self.locks)
            current = self.locks.get(name)
            if action == 'acquire':
                if not isinstance(ttl, (int, float)) or not LOCK_TTL[0] <= ttl <= LOCK_TTL[1]:
                    raise ValueError('ttl must be %d..%d seconds' % LOCK_TTL)
                if not isinstance(reason, str) or not reason.strip() or len(reason) > 200:
                    raise ValueError('reason 1..200 characters required')
                if current and current['holder'] != holder:
                    raise LockBusy(current)
                self.locks[name] = dict(name=name, holder=holder, reason=reason,
                                        since=current['since'] if current else now, expires=now + ttl)
                result = dict(self.locks[name], acquired=True)
            elif action == 'release':
                if current and current['holder'] != holder:
                    raise LockBusy(current)
                self.locks.pop(name, None)
                result = dict(name=name, released=current is not None)
            else:
                raise ValueError('lock action must be acquire, release or list')
            try:
                self.persist()
            except OSError:
                self.locks = old
                raise
            return result

    def events(self, after, wait):
        deadline = time.monotonic() + wait
        with self.condition:
            while True:
                events = [m for m in self.messages if m['seq'] > after][:100]
                remaining = deadline - time.monotonic()
                if events or remaining <= 0:
                    return events
                self.condition.wait(remaining)


def make_server(host, port, journal, token, artifact_dir=None):
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
            if url.path == '/artifact' and artifact_dir is not None:
                name = urllib.parse.parse_qs(url.query).get('name', [''])[0]
                if (not name or Path(name).name != name or '/' in name or '\\' in name
                        or Path(name).suffix not in ('.png', '.log', '.txt')):
                    return self.reply(400, {'error': 'invalid artifact filename'})
                path = Path(artifact_dir) / name
                if not path.is_file() or path.is_symlink():
                    return self.reply(404, {'error': 'artifact not found'})
                return self.reply(200, path.read_bytes(), 'application/octet-stream')
            if url.path == '/health':
                return self.reply(200, {'ok': True, 'messages': len(journal.messages), 'protocol': PROTOCOL,
                                        'last_seq': journal.last_seq})
            if url.path == '/locks':
                return self.reply(200, {'locks': journal.lock('list', None, None)})
            if url.path != '/events':
                return self.reply(404, {'error': 'not found'})
            try:
                args = urllib.parse.parse_qs(url.query)
                after = int(args.get('after', ['0'])[0])
                wait = int(args.get('wait', ['0'])[0])
                if not 0 <= after <= journal.last_seq or not 0 <= wait <= 25:
                    raise ValueError()
            except ValueError:
                return self.reply(400, {'error': 'invalid cursor or wait'})
            self.reply(200, {'events': journal.events(after, wait)})

        def do_POST(self):
            if not self.authorized():
                return
            if self.path not in ('/messages', '/archive', '/locks'):
                return self.reply(404, {'error': 'not found'})
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= MAX_BODY:
                    raise ValueError('message too large or empty')
                data = json.loads(self.rfile.read(length))
                if not isinstance(data, dict):
                    raise ValueError('expected object')
                if self.path == '/archive':
                    result = journal.remove(data.get('seq'), data.get('peer'), data.get('digest'))
                elif self.path == '/locks':
                    result = journal.lock(data.get('action'), data.get('name'), data.get('holder'),
                                          data.get('ttl'), data.get('reason', ''))
                else:
                    result = journal.append(data)
            except LockBusy as e:
                return self.reply(409, {'error': str(e), 'lock': e.lock})
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


def request_bytes(config, route, payload=None):
    """Re-resolve on bounded transport retries; IDs/digests make mutation retries safe."""
    for attempt in range(3):
        req = urllib.request.Request(config['url'].rstrip('/') + route, data=payload,
                                     headers={'Authorization': 'Bearer ' + config['token'],
                                              'Content-Type': 'application/json'})
        # No environment proxy or persistent connection: resolve the LAN hostname on each attempt.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(req, timeout=35) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code < 500 or attempt == 2:
                raise
            error.close()
        except (OSError, http.client.IncompleteRead):
            if attempt == 2:
                raise
        time.sleep(.5 * (attempt + 1))


def request(config, route, data=None):
    payload = None if data is None else json.dumps(data).encode()
    return json.loads(request_bytes(config, route, payload))


def receive(config, cursor_path, once=False, continuous=False, config_path=None):
    cursor_path = Path(cursor_path)
    cursor = json.loads(cursor_path.read_text())['after'] if cursor_path.exists() else 0
    inbox = cursor_path.with_suffix('.inbox.jsonl')
    outage = False
    while True:
        try:
            if config_path is not None:
                updated = json.loads(Path(config_path).read_text(encoding='utf-8'))
                if updated['peer'] != config['peer']:
                    raise ValueError('cannot change peer identity while a watcher is running')
                config = updated  # Reload endpoint/token on retry; urllib resolves the hostname for each request.
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


def history_messages(path):
    path = Path(path)
    if not path.exists():
        return []
    return [json.loads(base64.b64decode(encoded)) for encoded in
            re.findall(r'^<!-- mailbox-event:([A-Za-z0-9+/=]+) -->$', path.read_text(encoding='utf-8'), re.M)]


def archive_message(path, item):
    path = Path(path)
    existing = history_messages(path)
    previous = next((m for m in existing if m['seq'] == item['seq']), None)
    if previous:
        if previous != item:
            raise ValueError('history has different contents for this sequence')
        return
    text = path.read_text(encoding='utf-8') if path.exists() else '# Agent mailbox history\n\n'
    encoded = base64.b64encode(json.dumps(item, ensure_ascii=False).encode()).decode()
    text += ('## #{seq} {sender} → {to} — {time}\n\n'.format(**item)
             + '\n'.join('> ' + line for line in item['text'].splitlines())
             + '\n\n<!-- mailbox-event:' + encoded + ' -->\n\n')
    replace_text(path, text)


def all_events(config, after=0):
    result = []
    while True:
        events = request(config, '/events?' + urllib.parse.urlencode({'after': after, 'wait': 0}))['events']
        result.extend(events)
        if len(events) < 100:
            return result
        after = events[-1]['seq']


def pending_messages(config, handled_path):
    """Tasks survive removal from the shared queue: the Markdown history is authoritative locally."""
    handled_path = Path(handled_path)
    after = json.loads(handled_path.read_text())['through'] if handled_path.exists() else 0
    history = history_messages(handled_path.with_name(handled_path.name.replace('.handled.json', '.history.md')))
    merged = {m['seq']: m for m in history + all_events(config, after)
              if m['to'] == config['peer'] and m['seq'] > after}
    return [merged[seq] for seq in sorted(merged)]


def collect(config, config_path, acknowledge=True):
    handled_path = Path(config_path).with_suffix('.handled.json')
    history_path = handled_path.with_name(handled_path.name.replace('.handled.json', '.history.md'))
    for item in all_events(config):
        if item['to'] != config['peer']:
            continue
        archive_message(history_path, item)  # Flush/replace successfully BEFORE the shared copy is removed.
        request(config, '/archive', dict(seq=item['seq'], peer=config['peer'], digest=digest(item)))
        print('Archived #%d to %s; removed shared copy' % (item['seq'], history_path), flush=True)
    if acknowledge:
        for item in pending_messages(config, handled_path):
            if item['text'].lstrip().upper().startswith('REQUEST'):
                request(config, '/messages', dict(id='archive-ack-' + config['peer'] + '-' + str(item['seq']),
                        sender=config['peer'], to=item['sender'],
                        text='ACK #%d: copied to local Markdown history; working on your request.' % item['seq']))
    return history_path


def mark_handled(config, handled_path, seq):
    pending = pending_messages(config, handled_path)
    if not pending or pending[0]['seq'] != seq:
        raise ValueError('complete only the oldest pending incoming message; inspect pending first')
    write_json(handled_path, {'through': seq})


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', default='build/agent-mailbox/config.json')
    sub = parser.add_subparsers(dest='command', required=True)
    init = sub.add_parser('init')
    init.add_argument('--url', required=True)
    init.add_argument('--peer', required=True, help='This agent\'s own identity: mac, pc, pi or HOST-AGENT (pc-codex)')
    init.add_argument('--token')
    serve = sub.add_parser('serve')
    serve.add_argument('--bind', default='127.0.0.1',
                       help='Use 0.0.0.0 on a trusted LAN to follow interface IP changes; default is loopback only')
    serve.add_argument('--port', type=int, default=50380)
    serve.add_argument('--journal', default='build/agent-mailbox/shared.jsonl')
    serve.add_argument('--artifacts', default='build/agent-mailbox/evidence')
    send = sub.add_parser('send')
    send.add_argument('--to', required=True, help='Recipient identity: one agent (pc-claude), or a legacy host (pc)')
    content = send.add_mutually_exclusive_group(required=True)
    content.add_argument('--text')
    content.add_argument('--file', type=Path)
    send.add_argument('--id', default=None, help='Reuse this id when retrying an uncertain send')
    fetch = sub.add_parser('fetch', help='Download a named authenticated evidence PNG/log/text file')
    fetch.add_argument('--name', required=True)
    fetch.add_argument('--out', type=Path, required=True)
    sub.add_parser('status')
    sub.add_parser('inbox')
    collector = sub.add_parser('collect', help='Archive incoming messages to Markdown, remove shared copies and ACK requests')
    collector.add_argument('--no-ack', action='store_true', help='For migration of messages already acknowledged')
    sub.add_parser('pending', help='List unhandled messages even if watch/inbox already delivered them')
    done = sub.add_parser('done', help='Mark the oldest pending message handled after its work/result is complete')
    done.add_argument('--seq', type=int, required=True)
    locker = sub.add_parser('lock', help='Acquire or release a shared-resource lock (pi-deploy, ksp-gamedata, '
                                         'board-flash, push-main) before using that resource')
    locker.add_argument('action', choices=('acquire', 'release'))
    locker.add_argument('name')
    locker.add_argument('--ttl', type=int, default=1800, help='Seconds until it expires by itself (60..14400)')
    locker.add_argument('--reason', default='')
    sub.add_parser('locks', help='List held locks with holder, reason and expiry')
    watch = sub.add_parser('watch')
    watch.add_argument('--once', action='store_true', help='Exit when an incoming message arrives; re-arm after handling it')
    args = parser.parse_args(argv)
    config_path = Path(args.config)
    if args.command == 'init':
        if config_path.exists():
            parser.error('configuration already exists; use another path instead of rotating a live token')
        if not valid_identity(args.peer):
            parser.error('identity must be mac, pc, pi or HOST-AGENT such as pc-codex')
        parsed = urllib.parse.urlsplit(args.url)
        if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.query or parsed.fragment:
            parser.error('expected an HTTP(S) server URL without credentials or query')
        write_json(config_path, {'url': args.url.rstrip('/'), 'peer': args.peer,
                                 'token': args.token or secrets.token_urlsafe(32)})
        print('Created ' + str(config_path) + ' (private; do not commit)')
        return
    config = json.loads(config_path.read_text(encoding='utf-8'))
    if args.command == 'serve':
        server = make_server(args.bind, args.port, Journal(args.journal), config['token'], args.artifacts)
        print('MAILBOX listening on %s:%s; journal %s' % (*server.server_address, args.journal), flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()
    elif args.command == 'lock':
        try:
            result = request(config, '/locks', dict(action=args.action, name=args.name, holder=config['peer'],
                                                    ttl=args.ttl, reason=args.reason))
        except urllib.error.HTTPError as error:
            if error.code != 409:
                raise
            detail = json.loads(error.read() or b'{}')
            error.close()
            raise ValueError(detail.get('error', 'lock busy'))
        print(json.dumps(result))
    elif args.command == 'locks':
        for lock in request(config, '/locks')['locks']:
            print('LOCK {name} held by {holder} until {until}: {reason}'.format(
                until=datetime.fromtimestamp(lock['expires'], timezone.utc).isoformat(), **lock))
    elif args.command == 'send':
        if not valid_identity(args.to):
            parser.error('recipient must be mac, pc, pi or HOST-AGENT such as pc-codex')
        body = args.text if args.text is not None else args.file.read_text(encoding='utf-8')
        item = request(config, '/messages', {'id': args.id or str(uuid.uuid4()), 'sender': config['peer'],
                                           'to': args.to, 'text': body})
        print('Stored message #{seq} ({id}); receipt is not an acknowledgement from the other agent'.format(**item))
    elif args.command == 'fetch':
        payload = request_bytes(config, '/artifact?' + urllib.parse.urlencode({'name': args.name}))
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(payload)
        print('Saved evidence to ' + str(args.out))
    elif args.command == 'status':
        print(json.dumps(request(config, '/health')))
    elif args.command == 'collect':
        collect(config, config_path, acknowledge=not args.no_ack)
    elif args.command == 'pending':
        pending = pending_messages(config, config_path.with_suffix('.handled.json'))
        print('MAILBOX: %d pending incoming message(s)' % len(pending))
        for item in pending:
            print('PENDING #{seq} {sender} [{time}]\n{text}'.format(**item), flush=True)
    elif args.command == 'done':
        mark_handled(config, config_path.with_suffix('.handled.json'), args.seq)
        print('Marked message #%d handled' % args.seq)
    else:
        receive(config, config_path.with_suffix('.cursor.json'), once=getattr(args, 'once', False),
                continuous=args.command == 'watch', config_path=config_path)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError) as error:
        print('MAILBOX error: ' + str(error), file=sys.stderr)
        sys.exit(1)
