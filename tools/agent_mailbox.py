#!/usr/bin/env python3
"""Durable, authenticated LAN mailbox for human-authorized agent coordination (stdlib only).

Protocol 4: registered agents with exclusive session leases, recipients expanded at send time, per-recipient
delivery state (queued, delivered, archived, acked, done) kept on the server until every recipient finished,
overdue detection, reassignment and a checksummed append-only log. Protocol 3 clients keep working."""
import argparse
import base64
import copy
import hashlib
from datetime import datetime, timezone
import hmac
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
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

PEERS = ('mac', 'pc')  # legacy protocol 2/3 host inboxes, drained once with collect --legacy-inbox
HOSTS = ('mac', 'pc', 'pi')  # in protocol 4 a bare host name is a group: every agent registered on that host
MAX_BODY = 16384
PROTOCOL = 4
# An identity is a host, optionally with the agent on it: mac, pc, pi, pc-claude, pc-codex, mac-claude ...
# Every agent uses its own identity and config (own session, history and done files); two agents never share one.
IDENTITY = re.compile(r'^(mac|pc|pi)(-[a-z0-9]{1,20})?$')
AGENT = re.compile(r'^(mac|pc|pi)-[a-z0-9]{1,20}$')  # only HOST-AGENT identities register (protocol 4)
SESSION = re.compile(r'^[A-Za-z0-9_-]{16,64}$')
LOCK_NAME = re.compile(r'^[a-z0-9][a-z0-9._-]{0,39}$')
LOCK_TTL = (60, 14400)
LEASE = 90  # seconds; watch/collect renew it about every 25 s
HEARTBEAT_SAVE = 20  # persist a renewed lease at most this often per agent
GONE_AFTER = 3600  # an expired lease older than this lists the agent as gone instead of idle
OVERDUE_ARCHIVE = 600  # delivered but not archived
OVERDUE_ACK = 900  # REQUEST not acked by its owner
COMPACT_AFTER = 1000  # appended records before the log is rewritten
BACKUPS_KEPT = 14
STATES = ('queued', 'delivered', 'archived', 'acked', 'done')
ORDER = {name: index for index, name in enumerate(STATES)}
KINDS = ('REQUEST', 'ACK', 'RESULT', 'NOTICE')  # anything else is INFO
ALLOWLIST = 'allowlist.json'  # next to the journal (and server.json): a JSON list of identities that may register
GATE_REFUSAL = 'identity not on the allowlist; see agent-bus-handbook docs/onboarding.rst'


def valid_identity(name):
    return isinstance(name, str) and bool(IDENTITY.match(name))


def host_of(identity):
    return identity.split('-', 1)[0]


def kind_of(text):
    word = re.match(r'\s*([A-Za-z]+)', text)
    word = word.group(1).upper() if word else ''
    return word if word in KINDS else 'INFO'


def iso(stamp):
    return datetime.fromtimestamp(stamp, timezone.utc).isoformat()


class Conflict(ValueError):
    """The request is valid but collides with another agent's state (HTTP 409)."""
    detail = {}


class NotFound(ValueError):
    """Unknown message id (HTTP 404)."""


class Forbidden(ValueError):
    """The register gate refuses a new identity (HTTP 403)."""


class LockBusy(Conflict):
    """A shared resource lock is held by another agent."""
    def __init__(self, lock):
        super().__init__('lock %s is held by %s until %s (%s)' % (
            lock['name'], lock['holder'], iso(lock['expires']), lock['reason']))
        self.lock = lock
        self.detail = {'lock': lock}


class MailboxError(ValueError):
    """A server refusal (4xx) with its HTTP status and message."""
    def __init__(self, code, message):
        super().__init__('HTTP %d: %s' % (code, message))
        self.code = code


def private_temp(path):
    """A temp file next to path, unique per process and call: two clients of one identity (an open session's
    watch and the wake daemon's) must never truncate each other's half-written temp file, which once left a
    0-byte cursor behind (mailbox #643)."""
    return path.with_name('%s.%d.%s.tmp' % (path.name, os.getpid(), uuid.uuid4().hex[:8]))


def replace_file(temp, path, attempts=20):
    """os.replace with retries: on Windows it fails with WinError 5/32 while another process has path open."""
    for attempt in range(attempts):
        try:
            temp.replace(path)
            return
        except PermissionError:
            if attempt == attempts - 1:
                try:
                    temp.unlink()
                except OSError:
                    pass
                raise
            time.sleep(0.05 * (attempt + 1))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = private_temp(path)
    with temp.open('w', encoding='utf-8') as f:
        os.chmod(temp, 0o600)
        json.dump(value, f, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    replace_file(temp, path)


def read_json(path, default):
    """Client state (cursor, track, done, session): an empty or invalid file is moved aside as
    *.corrupt-<time> with a warning and reads as the default, so one bad file cannot stop every watch."""
    path = Path(path)
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (ValueError, UnicodeDecodeError) as error:
        aside = path.with_name('%s.corrupt-%s' % (path.name, datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')))
        try:
            path.replace(aside)
            where = 'moved to %s' % aside.name
        except OSError:
            where = 'left in place'
        print('MAILBOX warning: unreadable state file %s (%s), %s; using the default' % (path, error, where),
              file=sys.stderr)
        return default


def digest(data):
    return hashlib.sha256(json.dumps(data, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def core_digest(item):
    """Identifies a message's immutable contents; every per-recipient state change carries it."""
    return digest({key: item[key] for key in ('seq', 'id', 'sender', 'text')})


def send_digest(sender, to, text, owner=None):
    """Retried sends must repeat these fields; the protocol 3 form omits owner."""
    data = dict(sender=sender, to=to, text=text)
    if owner is not None:
        data['owner'] = owner
    return digest(data)


def replace_text(path, text):
    replace_bytes(path, text.encode('utf-8'))


def fsync_dir(path):
    if os.name == 'nt':
        return
    fd = os.open(str(path), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def replace_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = private_temp(path)
    with temp.open('wb') as f:
        os.chmod(temp, 0o600)
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    replace_file(temp, path)
    fsync_dir(path.parent)


def record_sum(record):
    body = {key: value for key, value in record.items() if key != 'sum'}
    return hashlib.sha256(json.dumps(body, sort_keys=True, ensure_ascii=False,
                                     separators=(',', ':')).encode()).hexdigest()[:16]


def encode_record(record):
    record = {key: value for key, value in record.items() if key != 'sum'}
    record['sum'] = record_sum(record)
    return (json.dumps(record, ensure_ascii=False, separators=(',', ':')) + '\n').encode()


def split_lines(raw):
    parts = raw.split(b'\n')
    return [part + b'\n' for part in parts[:-1]] + ([parts[-1]] if parts[-1] else [])


def v3_view(m, recipient):
    return dict(seq=m['seq'], id=m['id'], sender=m['sender'], to=recipient, text=m['text'], time=m['time'])


def entry_state(entry):
    return 'reassigned' if entry.get('reassigned') else entry['state']


class Journal:
    """Checksummed append-only log of protocol 4 records, compacted by atomic replacement.

    The first record is a header snapshot (version, last sequence, receipts, locks, agents); each following
    record upserts a message, drops a finished one into a receipt, or changes a lock or an agent. Protocol 2/3
    journals (plain JSONL with an unchecked header) are migrated on load after a backup copy."""
    def __init__(self, path, allowlist=None):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        # The register gate: re-read on every register so the operator edits it without a restart.
        self.allowlist_path = Path(allowlist) if allowlist else self.path.parent / ALLOWLIST
        self.backup_dir = self.path.parent / 'backup'
        self.condition = threading.Condition()
        self.messages = {}  # id -> message, in sequence order
        self.receipts = {}  # id -> compact receipt of a finished (or protocol 3 removed) message
        self.locks = {}  # name -> {name, holder, reason, since, expires}: shared resources (protocol 3)
        self.agents = {}  # identity -> {identity, host, tool, cwd, session, registered, last_seen, lease_expires}
        self.last_seq = self.rev = 0
        self._saved = {}
        self._records = 0
        self._fh = None
        self._backup_day = None
        migrated = None
        if self.path.exists() and self.path.stat().st_size:
            raw = self.path.read_bytes()
            try:
                head = json.loads(raw.split(b'\n', 1)[0])
            except ValueError:
                head = None
            if isinstance(head, dict) and head.get('k') == 'header' and 'sum' in head:
                self._replay(raw)
            else:
                migrated = self._load_legacy(raw)
        if migrated is not None:
            stamp = datetime.now().strftime('%Y%m%dT%H%M%S')
            replace_bytes(self.backup_dir / ('%s.v%d-%s%s' % (self.path.stem, migrated, stamp, self.path.suffix)),
                          self.path.read_bytes())
        if migrated is not None or not self._records or self._records > COMPACT_AFTER:
            self.compact()
        else:
            self._open()
        self._daily_backup()

    # -- log -------------------------------------------------------------------------------------------------
    def _replay(self, raw):
        good = 0
        lines = split_lines(raw)
        for index, line in enumerate(lines):
            try:
                record = json.loads(line)
                if not isinstance(record, dict) or record.get('sum') != record_sum(record):
                    raise ValueError('checksum mismatch')
                if (index == 0) != (record.get('k') == 'header'):
                    raise ValueError('header misplaced')
                if index == 0 and record.get('mailbox_version') != PROTOCOL:
                    raise ValueError('unsupported version')
                self._apply(record)
            except (ValueError, KeyError, TypeError) as error:
                if index == len(lines) - 1 and not line.endswith(b'\n'):
                    break  # torn final record from an interrupted append
                raise ValueError('corrupt mailbox journal: record %d (%s)' % (index + 1, error))
            good += len(line)
            self._records += 1
        if good != len(raw):
            with self.path.open('r+b') as f:
                f.truncate(good)
                f.flush()
                os.fsync(f.fileno())

    def _load_legacy(self, raw):
        """Protocol 2/3 header + messages, or the older headerless append-only form; returns its version."""
        good, previous, version, receipts, items = 0, 0, 1, {}, []
        for line in split_lines(raw):
            try:
                item = json.loads(line)
                if not good and item.get('mailbox_version') in (2, 3):
                    version = item['mailbox_version']
                    self.last_seq, receipts = item['last_seq'], dict(item['receipts'])
                    self.locks = dict(item.get('locks', {}))
                else:
                    if item['seq'] <= previous:
                        raise ValueError('non-increasing sequence')
                    previous = item['seq']
                    items.append(item)
                    self.last_seq = max(self.last_seq, item['seq'])
                    receipts[item['id']] = dict(seq=item['seq'], to=item['to'], digest=send_digest(
                        item['sender'], item['to'], item['text']))
            except (ValueError, KeyError, TypeError, AttributeError):
                if good + len(line) != len(raw) or line.endswith(b'\n'):
                    raise ValueError('corrupt mailbox journal')
                break
            good += len(line)
        for item in items:
            try:
                created = datetime.fromisoformat(item['time']).timestamp()
            except (TypeError, ValueError):
                created = time.time()
            kind = kind_of(item['text'])
            self.rev += 1
            self.messages[item['id']] = dict(
                seq=item['seq'], id=item['id'], sender=item['sender'], to=[item['to']], address=item['to'],
                owner=item['to'] if kind == 'REQUEST' else None, kind=kind, text=item['text'], time=item['time'],
                created=created, deadline=None, send_digest=send_digest(item['sender'], item['to'], item['text']),
                rev=self.rev, state={item['to']: dict(state='queued', queued=created)})
        self.receipts = {key: value for key, value in receipts.items() if key not in self.messages}
        return version

    def _apply(self, record):
        kind = record['k']
        if kind == 'header':
            self.last_seq, self.rev = record['last_seq'], record['rev']
            self.receipts, self.locks = dict(record['receipts']), dict(record['locks'])
            self.agents = dict(record['agents'])
        elif kind == 'msg':
            m = record['m']
            self.messages[m['id']] = m
            self.last_seq, self.rev = max(self.last_seq, m['seq']), max(self.rev, m['rev'])
        elif kind == 'drop':
            del self.messages[record['id']]
            self.receipts[record['id']] = record['receipt']
            self.rev = max(self.rev, record['receipt']['rev'])
        elif kind == 'lock':
            if record['lock'] is None:
                self.locks.pop(record['name'], None)
            else:
                self.locks[record['name']] = record['lock']
        elif kind == 'agent':
            self.agents[record['a']['identity']] = record['a']
        else:
            raise ValueError('unknown record kind')

    def _open(self):
        self._fh = self.path.open('ab')
        try:
            os.chmod(self.path, 0o600)
        except OSError:
            pass

    def close(self):
        with self.condition:
            if self._fh is not None:
                self._fh.close()
                self._fh = None

    def _commit(self, record):
        """Append one checksummed record and fsync it, then apply it in memory: a failed write changes nothing."""
        data = encode_record(record)
        fd = self._fh.fileno()
        size = os.fstat(fd).st_size
        try:
            self._fh.write(data)
            self._fh.flush()
            os.fsync(fd)
        except OSError:
            try:
                os.ftruncate(fd, size)
            except OSError:
                pass
            raise
        self._records += 1
        self._apply(record)

    def compact(self):
        """Rewrite the log as one header snapshot plus the active messages, atomically under the lock."""
        with self.condition:
            records = [dict(k='header', mailbox_version=PROTOCOL, last_seq=self.last_seq, rev=self.rev,
                            receipts=self.receipts, locks=self.locks, agents=self.agents)]
            records += [dict(k='msg', m=m) for m in self.messages.values()]
            if self._fh is not None:
                self._fh.close()
                self._fh = None
            try:
                replace_bytes(self.path, b''.join(encode_record(r) for r in records))
                self._records = len(records)
            finally:
                self._open()

    def _daily_backup(self, now=None):
        day = datetime.fromtimestamp(time.time() if now is None else now).strftime('%Y-%m-%d')
        if day == self._backup_day:
            return
        self._backup_day = day
        target = self.backup_dir / ('%s-%s%s' % (self.path.stem, day, self.path.suffix))
        try:
            if not target.exists():
                replace_bytes(target, self.path.read_bytes())
            old = sorted(self.backup_dir.glob('%s-????-??-??%s' % (self.path.stem, self.path.suffix)))
            for path in old[:-BACKUPS_KEPT]:
                path.unlink()
        except OSError as error:
            print('MAILBOX backup failed: %s' % error, file=sys.stderr, flush=True)

    def _after_change(self, now):
        if self._records > COMPACT_AFTER:
            self.compact()
        self._daily_backup(now)

    # -- agents ----------------------------------------------------------------------------------------------
    def _agent_view(self, agent, now):
        view = {key: value for key, value in agent.items() if key != 'session'}
        if agent.get('released'):
            view['state'] = 'gone'
        elif agent['lease_expires'] > now:
            view['state'] = 'active'
        else:
            view['state'] = 'idle' if now - agent['last_seen'] < GONE_AFTER else 'gone'
        return view

    def register_gate(self):
        """(state, names, problem): ('off', None, None) without an allowlist file, ('on', set, None) for a valid
        list of HOST-AGENT names, ('invalid', None, reason) otherwise; an invalid file refuses NEW identities."""
        try:
            raw = self.allowlist_path.read_bytes()
        except FileNotFoundError:
            return 'off', None, None
        except OSError as error:
            return 'invalid', None, 'cannot read %s: %s' % (self.allowlist_path.name, error.strerror or error)
        try:
            names = json.loads(raw.decode('utf-8-sig'))
        except (UnicodeDecodeError, ValueError) as error:
            return 'invalid', None, '%s is not valid JSON: %s' % (self.allowlist_path.name, error)
        if not isinstance(names, list) or not all(isinstance(n, str) and AGENT.match(n) for n in names):
            return 'invalid', None, '%s must be a JSON list of HOST-AGENT identities' % self.allowlist_path.name
        return 'on', set(names), None

    def gate_summary(self):
        state, names, problem = self.register_gate()
        if state == 'on':
            return 'on (%d identities)' % len(names)
        return 'off' if state == 'off' else 'on, invalid allowlist: new identities refused (%s)' % problem

    def _check_gate(self, identity):
        """Only NEW identities pass the gate: one already in the journal renews and takes over as before, so
        seeding or editing the allowlist can never lock out a live agent."""
        state, names, problem = self.register_gate()
        if state == 'off' or (state == 'on' and identity in names):
            return
        if state == 'invalid':
            print('MAILBOX register gate: refused new identity %s, %s' % (identity, problem),
                  file=sys.stderr, flush=True)
            raise Forbidden('register gate: the server allowlist is invalid (%s); new identities are refused '
                            'until the operator fixes it' % problem)
        print('MAILBOX register gate: refused new identity %s (not on the allowlist)' % identity,
              file=sys.stderr, flush=True)
        raise Forbidden(GATE_REFUSAL)

    def register(self, identity, session, tool, cwd, takeover=False, release=False, now=None):
        now = time.time() if now is None else now
        if not isinstance(identity, str) or not AGENT.match(identity):
            raise ValueError('register a HOST-AGENT identity such as pc-codex; bare mac/pc/pi are host groups')
        if not isinstance(session, str) or not SESSION.match(session):
            raise ValueError('session must be 16..64 characters [A-Za-z0-9_-]')
        if not isinstance(tool, str) or not 0 < len(tool) <= 40 or not isinstance(cwd, str) or len(cwd) > 400:
            raise ValueError('tool 1..40 and cwd up to 400 characters required')
        with self.condition:
            old = self.agents.get(identity)
            if old is None and not release:
                self._check_gate(identity)
            if old is not None and old.get('retired'):
                raise Conflict('identity %s was retired by %s on %s; only retire --undo brings it back'
                               % (identity, old.get('retired_by', '?'), iso(old['retired'])))
            same = old is not None and hmac.compare_digest(old['session'], session)
            if release:
                if not same:
                    raise Conflict('only the session holding %s can release it' % identity)
                agent = dict(old, released=now, lease_expires=now)
            else:
                if old is not None and not same:
                    who = '%s in %s, last seen %s' % (old['tool'], old['cwd'], iso(old['last_seen']))
                    if not old.get('released') and old['lease_expires'] > now:
                        raise Conflict('identity %s is in use by a live session (%s); never share an identity'
                                       % (identity, who))
                    if not takeover:
                        raise Conflict('identity %s belongs to another session whose lease ended (%s): confirm '
                                       'that agent is gone, then register --takeover' % (identity, who))
                agent = dict(identity=identity, host=host_of(identity), tool=tool, cwd=cwd, session=session,
                             registered=old['registered'] if same else now, last_seen=now,
                             lease_expires=now + LEASE, released=None)
            self._commit(dict(k='agent', a=agent))
            self._saved[identity] = now
            self._after_change(now)
            return self._agent_view(agent, now)

    def _touch(self, identity, session, now):
        """Authenticate an agent call by its session and renew the lease (heartbeat)."""
        agent = self.agents.get(identity) if isinstance(identity, str) else None
        if agent is None:
            raise ValueError('identity %s is not registered (protocol 4): run register' % identity)
        if not isinstance(session, str) or not SESSION.match(session) or not hmac.compare_digest(
                agent['session'], session):
            raise Conflict('identity %s belongs to another session (%s in %s); never share an identity'
                           % (identity, agent['tool'], agent['cwd']))
        if agent.get('retired'):
            raise Conflict('identity %s is retired; it no longer takes part in the mailbox' % identity)
        if agent.get('released'):
            raise Conflict('identity %s released its registration; run register again' % identity)
        agent = dict(agent, last_seen=now, lease_expires=now + LEASE)
        if now - self._saved.get(identity, 0) >= HEARTBEAT_SAVE:
            self._commit(dict(k='agent', a=agent))
            self._saved[identity] = now
        else:
            self.agents[identity] = agent
        return agent

    def agents_list(self, now=None, retired=False):
        """Registered identities; retired ones only when asked for (their history stays in the journal)."""
        now = time.time() if now is None else now
        with self.condition:
            return [self._agent_view(self.agents[key], now) for key in sorted(self.agents)
                    if retired or not self.agents[key].get('retired')]

    def retire(self, identity, session, target, undo=False, now=None):
        """Remove a dead identity from routing, agents and overdue, keeping its records and message history.

        Refused for a live lease and for an identity that still owns an unfinished REQUEST (reassign it first).
        Its copies of other unfinished messages are closed for it, so they no longer wait for it. ``undo``
        lifts the retirement; the identity then needs register --takeover from a new session."""
        now = time.time() if now is None else now
        with self.condition:
            self._touch(identity, session, now)
            agent = self.agents.get(target) if isinstance(target, str) else None
            if agent is None:
                raise NotFound('unknown identity %r: only registered identities can be retired' % target)
            if target == identity:
                raise ValueError('an identity cannot retire itself; use register --release')
            if undo:
                if agent.get('retired'):
                    agent = {key: value for key, value in agent.items() if key not in ('retired', 'retired_by')}
                    self._commit(dict(k='agent', a=agent))
                    self._after_change(now)
                return self._agent_view(agent, now)
            if agent.get('retired'):
                return self._agent_view(agent, now)
            if self._live_lease(target, now):
                raise Conflict('identity %s holds a live lease; it must release or expire before retiring' % target)
            owned = sorted('#%d %s' % (m['seq'], m['id']) for m in self.messages.values()
                           if m['kind'] == 'REQUEST' and m['owner'] == target
                           and not self._complete(m, target, m['state'][target]))
            if owned:
                raise Conflict('identity %s still owns unfinished REQUEST(s) %s: reassign them first'
                               % (target, ', '.join(owned)))
            agent = dict(agent, retired=now, retired_by=identity, released=agent.get('released') or now,
                         lease_expires=min(agent['lease_expires'], now))
            self._commit(dict(k='agent', a=agent))
            for m in list(self.messages.values()):
                entry = m['state'].get(target)
                if entry is None or self._complete(m, target, entry):
                    continue
                m = copy.deepcopy(m)
                m['state'][target]['retired'] = now
                m['rev'] = self.rev + 1
                self._commit(dict(k='msg', m=m))
                if all(self._complete(m, r, e) for r, e in m['state'].items()):
                    receipt = dict(seq=m['seq'], sender=m['sender'], kind=m['kind'], owner=m['owner'],
                                   digest=m['send_digest'], core=core_digest(m), rev=self.rev + 1, closed=now,
                                   recipients={r: entry_state(e) for r, e in m['state'].items()})
                    self._commit(dict(k='drop', id=m['id'], receipt=receipt))
            self.condition.notify_all()
            self._after_change(now)
            return self._agent_view(agent, now)

    def _live_lease(self, identity, now):
        agent = self.agents.get(identity)
        return agent is not None and not agent.get('released') and agent['lease_expires'] > now

    def _recipient(self, identity, recipient):
        if recipient in (None, '', identity):
            return identity
        if recipient in HOSTS and recipient == host_of(identity):
            return recipient  # drain the legacy protocol 3 inbox of the agent's own host
        raise ValueError('%s may only act for itself or its own legacy host inbox' % identity)

    # -- messages --------------------------------------------------------------------------------------------
    def _expand(self, address, sender):
        if not isinstance(address, str) or not 0 < len(address) <= 400:
            raise ValueError('to: one identity, a comma list, a host group (mac, pc, pi) or all')
        live = sorted(identity for identity, agent in self.agents.items() if not agent.get('released'))
        result = []
        for name in (part.strip() for part in address.split(',')):
            if name == 'all':
                group = live
            elif name in HOSTS:
                group = [identity for identity in live if host_of(identity) == name]
                if not group:
                    raise ValueError('no agents registered on host %s' % name)
            elif valid_identity(name) and name in live:
                group = [name]
            else:
                raise ValueError('unknown recipient %r: it must be registered (agents lists them); use send '
                                 '--legacy for a protocol 3 inbox' % name)
            for identity in group:
                if identity != sender and identity not in result:
                    result.append(identity)
        if not result:
            raise ValueError('no recipients besides the sender')
        return result

    def _existing(self, message_id, send_dig):
        m = self.messages.get(message_id)
        if m is None and message_id not in self.receipts:
            return None
        old = m['send_digest'] if m is not None else self.receipts[message_id].get('digest')
        if old != send_dig:
            raise ValueError('message id already used with different contents')
        return m if m is not None else dict(id=message_id, seq=self.receipts[message_id]['seq'], archived=True)

    def _new_message(self, message_id, sender, recipients, address, owner, text, deadline, send_dig, now):
        kind = kind_of(text)
        m = dict(seq=self.last_seq + 1, id=message_id, sender=sender, to=list(recipients), address=address,
                 owner=owner, kind=kind, text=text, time=iso(now), created=now, deadline=deadline,
                 send_digest=send_dig, rev=self.rev + 1,
                 state={r: dict(state='queued', queued=now) for r in recipients})
        self._commit(dict(k='msg', m=m))
        self.condition.notify_all()
        return m

    @staticmethod
    def _check_text(body, message_id):
        if (not isinstance(body, str) or not body.strip() or len(body) > 8000
                or not isinstance(message_id, str) or len(message_id) > 80 or not message_id):
            raise ValueError('text 1..8000 characters and id 1..80 characters required')

    def append(self, data, now=None):
        """Protocol 3 send: one literal identity (also a legacy host inbox), no session; returns its v3 view."""
        now = time.time() if now is None else now
        sender, recipient = data.get('sender'), data.get('to')
        body, message_id = data.get('text'), data.get('id')
        if not valid_identity(sender) or not valid_identity(recipient) or sender == recipient:
            raise ValueError('sender/to must be different identities (mac, pc, pi or HOST-AGENT such as pc-codex); '
                             'text 1..8000 characters; id required')
        self._check_text(body, message_id)
        with self.condition:
            send_dig = send_digest(sender, recipient, body)
            found = self._existing(message_id, send_dig)
            if found is not None:
                return v3_view(found, recipient) if 'state' in found else found
            owner = recipient if kind_of(body) == 'REQUEST' else None
            m = self._new_message(message_id, sender, [recipient], recipient, owner, body, None, send_dig, now)
            self._after_change(now)
            return v3_view(m, recipient)

    def send(self, data, now=None):
        """Protocol 4 send from a registered agent: recipients expanded and stored with the message."""
        now = time.time() if now is None else now
        sender, address, owner = data.get('sender'), data.get('to'), data.get('owner')
        body, message_id, deadline = data.get('text'), data.get('id'), data.get('deadline')
        self._check_text(body, message_id)
        if owner is not None and not valid_identity(owner):
            raise ValueError('owner must be an identity')
        if deadline is not None and (isinstance(deadline, bool) or not isinstance(deadline, (int, float))
                                     or not math.isfinite(deadline)):
            raise ValueError('deadline must be a UNIX time')
        with self.condition:
            self._touch(sender, data.get('session'), now)
            if not isinstance(address, str):
                raise ValueError('to required')
            send_dig = send_digest(sender, address, body, owner)
            found = self._existing(message_id, send_dig)
            if found is not None:
                return found
            recipients = self._expand(address, sender)
            if kind_of(body) == 'REQUEST':
                if owner is None:
                    if len(recipients) != 1:
                        raise ValueError('a REQUEST to several recipients needs exactly one --owner among them')
                    owner = recipients[0]
                elif owner not in recipients:
                    raise ValueError('owner %s is not among the recipients %s' % (owner, ','.join(recipients)))
            elif owner is not None:
                raise ValueError('--owner applies only to REQUEST messages')
            m = self._new_message(message_id, sender, recipients, address, owner, body, deadline, send_dig, now)
            self._after_change(now)
            return m

    @staticmethod
    def _complete(m, recipient, entry):
        if entry.get('reassigned') or entry.get('retired'):
            return True
        needed = 'done' if m['kind'] == 'REQUEST' and recipient == m['owner'] else 'archived'
        return ORDER[entry['state']] >= ORDER[needed]

    def _set_state(self, message_id, recipient, state, copied_digest, now):
        m = self.messages.get(message_id)
        if m is None:
            receipt = self.receipts.get(message_id)
            if receipt is None:
                return dict(id=message_id, recipient=recipient, state='unknown')
            if 'core' in receipt and copied_digest != receipt['core']:
                raise ValueError('archived copy does not match message %s' % message_id)
            states = receipt.get('recipients', {receipt.get('to'): 'archived'})
            return dict(id=message_id, recipient=recipient, state='closed', recipients=states)
        if copied_digest != core_digest(m):
            raise ValueError('recipient or archived copy does not match')
        entry = m['state'].get(recipient)
        if entry is None:
            raise ValueError('%s is not a recipient of %s' % (recipient, message_id))
        if ORDER[state] <= ORDER[entry['state']]:
            return dict(id=message_id, recipient=recipient, state=entry['state'], entry=entry)
        m = copy.deepcopy(m)
        entry = m['state'][recipient]
        for step in STATES[ORDER[entry['state']] + 1:ORDER[state] + 1]:
            if step != 'acked' or step == state:  # done does not claim an ACK that was never sent
                entry.setdefault(step, now)
        entry['state'] = state
        m['rev'] = self.rev + 1
        self._commit(dict(k='msg', m=m))
        self.condition.notify_all()
        if all(self._complete(m, r, e) for r, e in m['state'].items()):
            receipt = dict(seq=m['seq'], sender=m['sender'], kind=m['kind'], owner=m['owner'],
                           digest=m['send_digest'], core=core_digest(m), rev=self.rev + 1, closed=now,
                           recipients={r: entry_state(e) for r, e in m['state'].items()})
            self._commit(dict(k='drop', id=message_id, receipt=receipt))
        return dict(id=message_id, recipient=recipient, state=state, entry=entry)

    def set_state(self, identity, session, message_id, state, copied_digest, recipient=None, now=None):
        now = time.time() if now is None else now
        if state not in STATES[1:]:
            raise ValueError('state must be delivered, archived, acked or done')
        with self.condition:
            self._touch(identity, session, now)
            result = self._set_state(message_id, self._recipient(identity, recipient), state, copied_digest, now)
            self._after_change(now)
            return result

    def lookup(self, identity, session, ids, recipient=None, now=None):
        now = time.time() if now is None else now
        if not isinstance(ids, list) or len(ids) > 500 or not all(isinstance(i, str) for i in ids):
            raise ValueError('ids: up to 500 message ids')
        with self.condition:
            self._touch(identity, session, now)
            recipient = self._recipient(identity, recipient)
            result = {}
            for message_id in ids:
                m = self.messages.get(message_id)
                receipt = self.receipts.get(message_id, {})
                if m is not None and recipient in m['state']:
                    result[message_id] = entry_state(m['state'][recipient])
                elif 'recipients' in receipt:
                    result[message_id] = receipt['recipients'].get(recipient, 'unknown')
                else:
                    result[message_id] = 'archived' if receipt.get('to') == recipient else 'unknown'
            return result

    @staticmethod
    def _inbox_view(m, recipient):
        view = {key: m[key] for key in ('seq', 'id', 'sender', 'to', 'owner', 'kind', 'text', 'time', 'deadline')}
        view.update(recipient=recipient, entry=dict(m['state'][recipient]))
        return view

    def inbox(self, identity, session, recipient=None, after_rev=0, wait=0, now=None):
        """Not-done messages for one recipient (marking queued ones delivered) and the caller's own REQUESTs."""
        deadline = time.monotonic() + wait
        with self.condition:
            self._touch(identity, session, time.time() if now is None else now)
            recipient = self._recipient(identity, recipient)
            while self.rev <= after_rev:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self.condition.wait(remaining)
            stamp = time.time() if now is None else now
            for m in list(self.messages.values()):
                entry = m['state'].get(recipient)
                if entry and entry['state'] == 'queued' and not entry.get('reassigned'):
                    self._set_state(m['id'], recipient, 'delivered', core_digest(m), stamp)
            messages = [self._inbox_view(m, recipient) for m in self.messages.values()
                        if recipient in m['state'] and not m['state'][recipient].get('reassigned')
                        and m['state'][recipient]['state'] != 'done']
            tracked = [dict(id=m['id'], seq=m['seq'], closed=False, owner=m['owner'],
                            state={r: entry_state(e) for r, e in m['state'].items()})
                       for m in self.messages.values() if m['sender'] == identity and m['kind'] == 'REQUEST']
            tracked += [dict(id=key, seq=r['seq'], closed=True, owner=r.get('owner'), state=r['recipients'])
                        for key, r in self.receipts.items() if r.get('sender') == identity
                        and r.get('kind') == 'REQUEST' and r.get('rev', 0) > after_rev]
            self._after_change(stamp)
            return dict(messages=messages, tracked=tracked, rev=self.rev)

    def track(self, message_id):
        with self.condition:
            if message_id in self.messages:
                return dict(copy.deepcopy(self.messages[message_id]), closed=False)
            if message_id in self.receipts:
                return dict(self.receipts[message_id], id=message_id, closed=True)
        raise NotFound('unknown message id %s' % message_id)

    def overdue(self, now=None):
        now = time.time() if now is None else now
        found = []
        with self.condition:
            for m in self.messages.values():
                for recipient, entry in m['state'].items():
                    if self._complete(m, recipient, entry):
                        continue
                    owner = m['kind'] == 'REQUEST' and recipient == m['owner']
                    reasons = []
                    if entry['state'] == 'delivered' and now - entry['delivered'] > OVERDUE_ARCHIVE:
                        reasons.append('delivered %d min ago, not archived' % ((now - entry['delivered']) // 60))
                    if owner and ORDER[entry['state']] < ORDER['acked'] and now - entry['queued'] > OVERDUE_ACK:
                        reasons.append('REQUEST not acked after %d min' % ((now - entry['queued']) // 60))
                    if owner and m.get('deadline') and now > m['deadline']:
                        reasons.append('past its deadline %s' % iso(m['deadline']))
                    agent = self.agents.get(recipient)
                    if agent is not None and self._agent_view(agent, now)['state'] != 'active':
                        reasons.append('recipient lease expired')
                    for reason in reasons:
                        found.append(dict(seq=m['seq'], id=m['id'], sender=m['sender'], recipient=recipient,
                                          kind=m['kind'], state=entry['state'], reason=reason))
        return found

    def reassign(self, identity, session, message_id, new_owner, now=None):
        """Move an unfinished REQUEST to another registered owner and notify the old and new owner."""
        now = time.time() if now is None else now
        with self.condition:
            self._touch(identity, session, now)
            m = self.messages.get(message_id)
            if m is None:
                raise NotFound('no active message %s' % message_id)
            if m['kind'] != 'REQUEST':
                raise ValueError('only a REQUEST has an owner to reassign')
            agent = self.agents.get(new_owner) if isinstance(new_owner, str) else None
            if agent is None or agent.get('released'):
                raise ValueError('unknown recipient %r: the new owner must be registered' % new_owner)
            if new_owner == m['sender']:
                raise ValueError('the sender cannot own its own REQUEST')
            old = m['owner']
            if old == new_owner:
                return copy.deepcopy(m)
            m = copy.deepcopy(m)
            if old in m['state']:
                m['state'][old]['reassigned'] = now
            if new_owner in m['state']:
                m['state'][new_owner].pop('reassigned', None)
            else:
                m['to'].append(new_owner)
                m['state'][new_owner] = dict(state='queued', queued=now)
            m['owner'] = new_owner
            m.setdefault('reassigned', []).append(dict(owner=old, to=new_owner, by=identity, time=now))
            m['rev'] = self.rev + 1
            self._commit(dict(k='msg', m=m))
            self.condition.notify_all()
            notify = [r for r in (old, new_owner) if r and r != identity]
            if notify:
                text = 'NOTICE: REQUEST #%d (%s) from %s reassigned from %s to %s by %s.' % (
                    m['seq'], message_id, m['sender'], old, new_owner, identity)
                notice_id = 'reassign-%d-%d' % (m['seq'], len(m['reassigned']))
                if self._existing(notice_id, send_digest(identity, ','.join(notify), text)) is None:
                    self._new_message(notice_id, identity, notify, ','.join(notify), None, text, None,
                                      send_digest(identity, ','.join(notify), text), now)
            self._after_change(now)
            return copy.deepcopy(m)

    # -- protocol 3 interoperation ---------------------------------------------------------------------------
    def remove(self, seq, peer, copied_digest, now=None):
        """Protocol 3 archive: marks only that recipient archived; the message stays for the others."""
        now = time.time() if now is None else now
        if not isinstance(seq, int) or not valid_identity(peer):
            raise ValueError('unknown message or wrong recipient')
        with self.condition:
            m = next((m for m in self.messages.values() if m['seq'] == seq), None)
            if m is None:
                if any(r.get('seq') == seq and (r.get('to') == peer or peer in r.get('recipients', {}))
                       for r in self.receipts.values()):
                    return dict(removed=False, seq=seq)
                raise ValueError('unknown message or wrong recipient')
            if peer not in m['state'] or copied_digest != digest(v3_view(m, peer)):
                raise ValueError('recipient or archived copy does not match')
            if self._live_lease(peer, now):
                raise Conflict('%s is registered by a live protocol 4 session; only that agent collects it' % peer)
            if ORDER[m['state'][peer]['state']] >= ORDER['archived']:
                return dict(removed=False, seq=seq)
            self._set_state(m['id'], peer, 'archived', core_digest(m), now)
            self._after_change(now)
            return dict(removed=True, seq=seq)

    def events(self, after, wait):
        """Protocol 3 event list: one view per recipient that has not archived the message yet."""
        deadline = time.monotonic() + wait
        with self.condition:
            while True:
                events, count = [], 0
                for m in self.messages.values():
                    if m['seq'] <= after:
                        continue
                    views = [v3_view(m, r) for r, e in m['state'].items()
                             if ORDER[e['state']] < ORDER['archived'] and not e.get('reassigned')]
                    if views:
                        events.extend(views)
                        count += 1
                        if count >= 100:
                            break
                remaining = deadline - time.monotonic()
                if events or remaining <= 0:
                    return events
                self.condition.wait(remaining)

    # -- locks -----------------------------------------------------------------------------------------------
    def _prune(self, now):
        expired = [name for name, lock in self.locks.items() if lock['expires'] <= now]
        for name in expired:
            self._commit(dict(k='lock', name=name, lock=None))
        return bool(expired)

    def lock(self, action, name, holder, ttl=None, reason='', now=None):
        """Acquire (or renew, by the same holder), release or list shared-resource locks. A lock held by another
        agent raises LockBusy; it expires by itself after its TTL, so a crashed agent cannot block for ever."""
        now = time.time() if now is None else now
        with self.condition:
            self._prune(now)
            if action == 'list':
                return sorted(self.locks.values(), key=lambda l: l['name'])
            if not isinstance(name, str) or not LOCK_NAME.match(name) or not valid_identity(holder):
                raise ValueError('lock name [a-z0-9._-] up to 40 characters and a valid holder identity required')
            current = self.locks.get(name)
            if action == 'acquire':
                if isinstance(ttl, bool) or not isinstance(ttl, (int, float)) or not LOCK_TTL[0] <= ttl <= LOCK_TTL[1]:
                    raise ValueError('ttl must be %d..%d seconds' % LOCK_TTL)
                if not isinstance(reason, str) or not reason.strip() or len(reason) > 200:
                    raise ValueError('reason 1..200 characters required')
                if current and current['holder'] != holder:
                    raise LockBusy(current)
                lock = dict(name=name, holder=holder, reason=reason,
                            since=current['since'] if current else now, expires=now + ttl)
                self._commit(dict(k='lock', name=name, lock=lock))
                result = dict(lock, acquired=True)
            elif action == 'release':
                if current and current['holder'] != holder:
                    raise LockBusy(current)
                if current:
                    self._commit(dict(k='lock', name=name, lock=None))
                result = dict(name=name, released=current is not None)
            else:
                raise ValueError('lock action must be acquire, release or list')
            self._after_change(now)
            return result

    def health(self, now=None):
        now = time.time() if now is None else now
        with self.condition:
            active = sum(1 for a in self.agents.values() if self._agent_view(a, now)['state'] == 'active')
            return {'ok': True, 'protocol': PROTOCOL, 'messages': len(self.messages), 'last_seq': self.last_seq,
                    'agents': active, 'overdue': len(self.overdue(now)), 'register_gate': self.gate_summary()}


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

        def guarded(self, action):
            try:
                return self.reply(200, action())
            except NotFound as e:
                return self.reply(404, {'error': str(e)})
            except Forbidden as e:
                return self.reply(403, {'error': str(e)})
            except Conflict as e:
                return self.reply(409, dict(e.detail, error=str(e)))
            except (ValueError, TypeError) as e:
                return self.reply(400, {'error': str(e)})

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            # Bootstrap only this public script. Never expose directories, config or journal files.
            if url.path == '/client.py':
                return self.reply(200, Path(__file__).read_bytes(), 'text/x-python; charset=utf-8')
            if not self.authorized():
                return
            args = {key: values[-1] for key, values in urllib.parse.parse_qs(url.query).items()}
            if url.path == '/artifact' and artifact_dir is not None:
                name = args.get('name', '')
                if (not name or Path(name).name != name or '/' in name or '\\' in name
                        or Path(name).suffix not in ('.png', '.log', '.txt')):
                    return self.reply(400, {'error': 'invalid artifact filename'})
                path = Path(artifact_dir) / name
                if not path.is_file() or path.is_symlink():
                    return self.reply(404, {'error': 'artifact not found'})
                return self.reply(200, path.read_bytes(), 'application/octet-stream')
            if url.path == '/health':
                return self.reply(200, journal.health())
            if url.path == '/locks':
                return self.guarded(lambda: {'locks': journal.lock('list', None, None)})
            if url.path == '/agents':
                return self.reply(200, {'agents': journal.agents_list(retired=args.get('retired') == '1'),
                                        'register_gate': journal.gate_summary()})
            if url.path == '/overdue':
                return self.reply(200, {'overdue': journal.overdue()})
            if url.path == '/track':
                return self.guarded(lambda: journal.track(args.get('id', '')))
            if url.path == '/inbox':
                try:
                    after_rev, wait = int(args.get('after_rev', '0')), int(args.get('wait', '0'))
                    if after_rev < 0 or not 0 <= wait <= 25:
                        raise ValueError()
                except ValueError:
                    return self.reply(400, {'error': 'invalid after_rev or wait'})
                return self.guarded(lambda: journal.inbox(args.get('identity'), args.get('session'),
                                                          args.get('recipient'), after_rev, wait))
            if url.path != '/events':
                return self.reply(404, {'error': 'not found'})
            try:
                after = int(args.get('after', '0'))
                wait = int(args.get('wait', '0'))
                if not 0 <= after <= journal.last_seq or not 0 <= wait <= 25:
                    raise ValueError()
            except ValueError:
                return self.reply(400, {'error': 'invalid cursor or wait'})
            self.reply(200, {'events': journal.events(after, wait)})

        def do_POST(self):
            if not self.authorized():
                return
            routes = {
                '/messages': lambda d: journal.send(d) if d.get('v') == 4 else journal.append(d),
                '/archive': lambda d: journal.remove(d.get('seq'), d.get('peer'), d.get('digest')),
                '/locks': lambda d: journal.lock(d.get('action'), d.get('name'), d.get('holder'),
                                                 d.get('ttl'), d.get('reason', '')),
                '/register': lambda d: journal.register(d.get('identity'), d.get('session'), d.get('tool'),
                                                        d.get('cwd', ''), bool(d.get('takeover')),
                                                        bool(d.get('release'))),
                '/state': lambda d: journal.set_state(d.get('identity'), d.get('session'), d.get('id'),
                                                      d.get('state'), d.get('digest'), d.get('recipient')),
                '/lookup': lambda d: journal.lookup(d.get('identity'), d.get('session'), d.get('ids'),
                                                    d.get('recipient')),
                '/reassign': lambda d: journal.reassign(d.get('identity'), d.get('session'), d.get('id'),
                                                        d.get('to')),
                '/retire': lambda d: journal.retire(d.get('identity'), d.get('session'), d.get('target'),
                                                    bool(d.get('undo'))),
            }
            if self.path not in routes:
                return self.reply(404, {'error': 'not found'})
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= MAX_BODY:
                    raise ValueError('message too large or empty')
                data = json.loads(self.rfile.read(length))
                if not isinstance(data, dict):
                    raise ValueError('expected object')
            except ValueError as e:
                return self.reply(400, {'error': str(e)})
            self.guarded(lambda: routes[self.path](data))

    class Server(ThreadingHTTPServer):
        daemon_threads = True
        allow_reuse_address = True

        def finish_request(self, request, client_address):
            request.settimeout(35)
            super().finish_request(request, client_address)

    return Server((host, port), Handler)


# -- client ----------------------------------------------------------------------------------------------------
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


def refusal(error):
    """Turn a 4xx HTTPError into a MailboxError carrying the server's message."""
    try:
        message = json.loads(error.read() or b'{}').get('error', error.reason)
    except ValueError:
        message = str(error.reason)
    error.close()
    return MailboxError(error.code, message)


def paths(config_path):
    config_path = Path(config_path)
    return dict(session=config_path.with_suffix('.session.json'), history=config_path.with_suffix('.history.md'),
                done=config_path.with_suffix('.done.json'), handled=config_path.with_suffix('.handled.json'),
                cursor=config_path.with_suffix('.cursor.json'), track=config_path.with_suffix('.track.json'),
                inbox=config_path.with_suffix('.inbox.jsonl'))


def require_agent(config):
    if not AGENT.match(config.get('peer', '')):
        raise ValueError('protocol 4 needs a HOST-AGENT identity (pc-codex), not %r: init a config of your own '
                         'with the same url/token and drain this one with collect --legacy-inbox'
                         % config.get('peer'))


def register(config, config_path, tool=None, takeover=False, release=False):
    require_agent(config)
    session_path = paths(config_path)['session']
    saved = read_json(session_path, {})
    session = saved.get('session') or secrets.token_hex(16)
    tool = tool or saved.get('tool') or config['peer'].split('-', 1)[1]
    try:
        result = request(config, '/register', dict(identity=config['peer'], session=session, tool=tool,
                                                   cwd=os.getcwd(), takeover=takeover, release=release))
    except urllib.error.HTTPError as error:
        if error.code >= 500:
            raise
        raise refusal(error)
    write_json(session_path, dict(session=session, tool=tool))
    return result


def ensure_session(config, config_path):
    saved = read_json(paths(config_path)['session'], {})
    if not saved.get('session'):
        register(config, config_path)
        saved = read_json(paths(config_path)['session'], {})
    return saved['session']


def agent_request(config, config_path, route, data=None, query=None):
    """An identity-scoped call: adds identity/session (the heartbeat) and re-registers a lost registration."""
    require_agent(config)
    for attempt in (0, 1):
        session = ensure_session(config, config_path)
        auth = dict(identity=config['peer'], session=session)
        try:
            if data is None:
                return request(config, route + '?' + urllib.parse.urlencode(dict(query or {}, **auth)))
            return request(config, route, dict(data, **auth))
        except urllib.error.HTTPError as error:
            if error.code >= 500:
                raise
            failure = refusal(error)
            if attempt or failure.code != 400 or 'not registered' not in str(failure):
                raise failure
            register(config, config_path)  # same session: the server lost or never had the registration


def legacy_request(config, route, data):
    try:
        return request(config, route, data)
    except urllib.error.HTTPError as error:
        if error.code >= 500:
            raise
        raise refusal(error)


def send_message(config, config_path, to, text, owner=None, deadline=None, message_id=None, legacy=False):
    message_id = message_id or str(uuid.uuid4())
    if legacy:  # protocol 3 form: one literal identity, e.g. a legacy mac/pc inbox
        return legacy_request(config, '/messages', dict(id=message_id, sender=config['peer'], to=to, text=text))
    return agent_request(config, config_path, '/messages', dict(v=4, id=message_id, sender=config['peer'], to=to,
                                                                text=text, owner=owner, deadline=deadline))


def history_messages(path):
    path = Path(path)
    if not path.exists():
        return []
    return [json.loads(base64.b64decode(encoded)) for encoded in
            re.findall(r'^<!-- mailbox-event:([A-Za-z0-9+/=]+) -->$', path.read_text(encoding='utf-8'), re.M)]


def recipient_of(item):
    return item.get('recipient') or item['to']


def history_item(m):
    return {key: m[key] for key in ('seq', 'id', 'sender', 'to', 'owner', 'kind', 'text', 'time', 'deadline',
                                    'recipient')}


def archive_message(path, item):
    """Local history is keyed by message id and recipient; a retry never duplicates an entry."""
    path = Path(path)
    key = (item['id'], recipient_of(item))
    previous = next((m for m in history_messages(path) if (m['id'], recipient_of(m)) == key), None)
    if previous:
        if core_digest(previous) != core_digest(item):
            raise ValueError('history has different contents for message %s' % item['id'])
        return
    text = path.read_text(encoding='utf-8') if path.exists() else '# Agent mailbox history\n\n'
    encoded = base64.b64encode(json.dumps(item, ensure_ascii=False).encode()).decode()
    text += ('## #{seq} {sender} → {recipient} — {time}\n\n'.format(**dict(item, recipient=recipient_of(item)))
             + '\n'.join('> ' + line for line in item['text'].splitlines())
             + '\n\n<!-- mailbox-event:' + encoded + ' -->\n\n')
    replace_text(path, text)


def import_legacy_history(config_path, history_path, host):
    """Copy unhandled items of the old shared host config's history (protocol 3) into this agent's history."""
    config_path = Path(config_path)
    through = read_json(config_path.with_name(host + '.handled.json'), {}).get('through', 0)
    for item in history_messages(config_path.with_name(host + '.history.md')):
        if item.get('to') == host and item['seq'] > through:
            archive_message(history_path, dict(item, recipient=host))


def collect(config, config_path, acknowledge=True, legacy_inbox=False):
    """Archive every not-yet-archived incoming copy to the Markdown history, then record archived on the server
    (never deleting other recipients' copies); ACK each REQUEST this agent owns. Prints what it archived."""
    history_path = paths(config_path)['history']
    recipients = [config['peer']]
    if legacy_inbox:
        recipients.append(host_of(config['peer']))
        import_legacy_history(config_path, history_path, host_of(config['peer']))
    done = set(read_json(paths(config_path)['done'], {}).get('ids', []))
    for recipient in recipients:
        messages = agent_request(config, config_path, '/inbox', query=dict(recipient=recipient))['messages']
        for m in messages:
            if ORDER[m['entry']['state']] < ORDER['archived']:
                archive_message(history_path, history_item(m))  # flushed and replaced BEFORE the server records it
                agent_request(config, config_path, '/state', dict(id=m['id'], recipient=recipient, state='archived',
                                                                   digest=core_digest(m)))
                print('MAILBOX #{seq} {sender} -> {recipient} [{kind} {time}] archived to history\n{text}'.format(**m),
                      flush=True)
            if (acknowledge and m['kind'] == 'REQUEST' and m['owner'] == recipient
                    and ORDER[m['entry']['state']] < ORDER['acked'] and m['id'] not in done):
                legacy_request(config, '/messages', dict(
                    id='ack-%s-%d' % (config['peer'], m['seq']), sender=config['peer'], to=m['sender'],
                    text='ACK #%d (%s): copied to local history; %s is working on your request.'
                         % (m['seq'], m['id'], config['peer'])))
                agent_request(config, config_path, '/state', dict(id=m['id'], recipient=recipient, state='acked',
                                                                   digest=core_digest(m)))
    return history_path


def pending_messages(config, config_path):
    """Messages not done by this agent: local history plus server copies, minus local and server done marks
    (the server mark survives a lost local file). Protocol 3 history honours its old handled watermark."""
    files = paths(config_path)
    done = set(read_json(files['done'], {}).get('ids', []))
    through = read_json(files['handled'], {}).get('through', 0)
    merged = {}
    for item in history_messages(files['history']):
        if 'kind' not in item and item['seq'] <= through:
            continue
        merged[(item['id'], recipient_of(item))] = item
    on_server = set()
    for m in agent_request(config, config_path, '/inbox', query={})['messages']:
        merged.setdefault((m['id'], m['recipient']), history_item(m))
        on_server.add((m['id'], m['recipient']))
    candidates = {key: item for key, item in merged.items() if key[0] not in done}
    by_recipient = {}
    for key in candidates:
        if key not in on_server:
            by_recipient.setdefault(key[1], []).append(key[0])
    for recipient, ids in by_recipient.items():
        for start in range(0, len(ids), 500):
            states = agent_request(config, config_path, '/lookup', dict(ids=ids[start:start + 500],
                                                                        recipient=recipient))
            for message_id, state in states.items():
                if state == 'done':
                    candidates.pop((message_id, recipient), None)
    return sorted(candidates.values(), key=lambda item: (item['seq'], recipient_of(item)))


def mark_done(config, config_path, message_id=None, seq=None):
    """Mark one message done by id (or sequence), in any order, on the server and in the local done file."""
    items = [item for item in pending_messages(config, config_path)
             if (message_id is not None and item['id'] == message_id) or (seq is not None and item['seq'] == seq)]
    if not items:
        raise ValueError('message %s is not pending; inspect pending first' % (message_id or '#%s' % seq))
    for item in items:
        agent_request(config, config_path, '/state', dict(id=item['id'], recipient=recipient_of(item), state='done',
                                                           digest=core_digest(item)))
    done_path = paths(config_path)['done']
    ids = read_json(done_path, {}).get('ids', [])
    write_json(done_path, {'ids': ids + sorted({item['id'] for item in items} - set(ids))})
    return items


def watch(config, config_path, once=False, continuous=True):
    """Long-poll this agent's inbox (renewing its lease), print new incoming messages and the acked/done changes
    of its own REQUESTs. Reloads the private config on every poll; transport outages retry when continuous."""
    config_path = Path(config_path)
    files = paths(config_path)
    cursor = read_json(files['cursor'], {})
    first = 'rev' not in cursor  # a new watcher does not replay every old closed REQUEST
    rev, seen = cursor.get('rev', 0), set(cursor.get('seen', []))
    tracked_cache = read_json(files['track'], {})
    outage = False
    while True:
        try:
            if config_path.exists():
                updated = json.loads(config_path.read_text(encoding='utf-8'))
                if updated['peer'] != config['peer']:
                    raise Conflict('cannot change peer identity while a watcher is running')
                config = updated  # Reload endpoint/token on retry; urllib resolves the hostname for each request.
            data = agent_request(config, config_path, '/inbox',
                                 query=dict(after_rev=rev, wait=25 if continuous else 0))
            if outage:
                print('MAILBOX reconnected', file=sys.stderr, flush=True)
            outage = False
        except (MailboxError, Conflict):
            raise
        except (OSError, ValueError) as e:
            if not continuous or isinstance(e, urllib.error.HTTPError) and e.code < 500:
                raise
            if not outage:
                print('MAILBOX offline; retrying: ' + str(e), file=sys.stderr, flush=True)
            outage = True
            time.sleep(3)
            continue
        found = False
        for m in data['messages']:
            if m['id'] in seen:
                continue
            files['inbox'].parent.mkdir(parents=True, exist_ok=True)
            with files['inbox'].open('a', encoding='utf-8') as f:
                f.write(json.dumps(history_item(m), ensure_ascii=False) + '\n')
                f.flush()
                os.fsync(f.fileno())
            print('\aMAILBOX #{seq} {sender} -> {recipient} [{kind} {time}]\n{text}'.format(**m), flush=True)
            found = True
        current = {}
        for t in data['tracked']:
            before = tracked_cache.get(t['id'], {})
            for recipient, state in t['state'].items():
                if (state != before.get(recipient) and state in ('acked', 'done', 'reassigned')
                        and not (first and t['closed'])):
                    print('MAILBOX TRACK #%d %s: %s %s' % (t['seq'], t['id'], recipient, state), flush=True)
                    found = True
            if not t['closed']:
                current[t['id']] = t['state']
        tracked_cache, first = current, False
        seen = {m['id'] for m in data['messages']}
        rev = data['rev']
        write_json(files['cursor'], {'rev': rev, 'seen': sorted(seen)})
        write_json(files['track'], tracked_cache)
        if not continuous or once and found:
            return


def show_track(item):
    lines = ['TRACK #%d %s %s from %s owner=%s%s' % (item['seq'], item['id'], item.get('kind', '?'),
                                                    item.get('sender', '?'), item.get('owner'),
                                                    ' (closed)' if item.get('closed') else '')]
    for recipient, entry in (item.get('state') or {}).items():
        times = ', '.join('%s %s' % (s, iso(entry[s])) for s in STATES if s in entry)
        lines.append('  %s: %s (%s)' % (recipient, entry_state(entry), times))
    for recipient, state in (item.get('recipients') or {}).items():
        lines.append('  %s: %s' % (recipient, state))
    return '\n'.join(lines)


def show_overdue(items):
    return ['OVERDUE #{seq} {id} {kind} {sender} -> {recipient} ({state}): {reason}'.format(**i) for i in items]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', default='build/agent-mailbox/config.json')
    sub = parser.add_subparsers(dest='command', required=True)
    init = sub.add_parser('init')
    init.add_argument('--url', required=True)
    init.add_argument('--peer', required=True, help='This agent\'s own identity: HOST-AGENT such as pc-codex')
    init.add_argument('--token')
    serve = sub.add_parser('serve')
    serve.add_argument('--bind', default='127.0.0.1',
                       help='Use 0.0.0.0 on a trusted LAN to follow interface IP changes; default is loopback only')
    serve.add_argument('--port', type=int, default=50380)
    serve.add_argument('--journal', default='build/agent-mailbox/shared.jsonl')
    serve.add_argument('--artifacts', default='build/agent-mailbox/evidence')
    serve.add_argument('--allowlist', help='Register gate file (default: allowlist.json next to the journal); '
                       'without the file every identity may register')
    reg = sub.add_parser('register', help='Register this identity and take its exclusive session lease')
    reg.add_argument('--tool', help='claude, codex ... (default: the AGENT part of the identity)')
    reg.add_argument('--takeover', action='store_true', help='Only after the old session is confirmed dead')
    reg.add_argument('--release', action='store_true', help='Give the identity up so another session may take it')
    lister = sub.add_parser('agents', help='List registered identities, tool, last heartbeat and state')
    lister.add_argument('--retired', action='store_true', help='Also list retired identities')
    retiring = sub.add_parser('retire', help='Operator: drop a dead identity from agents/routing, keep history')
    retiring.add_argument('identity')
    retiring.add_argument('--undo', action='store_true', help='Lift the retirement (then register --takeover)')
    send = sub.add_parser('send')
    send.add_argument('--to', required=True, help='pc-claude, a comma list, a host group (mac, pc, pi) or all')
    send.add_argument('--owner', help='The one recipient that owns a REQUEST (required with several recipients)')
    send.add_argument('--deadline', type=float, help='Minutes until an unfinished REQUEST is overdue')
    send.add_argument('--legacy', action='store_true', help='Protocol 3 form to one literal identity (old mac/pc)')
    content = send.add_mutually_exclusive_group(required=True)
    content.add_argument('--text')
    content.add_argument('--file', type=Path)
    send.add_argument('--id', default=None, help='Reuse this id when retrying an uncertain send')
    fetch = sub.add_parser('fetch', help='Download a named authenticated evidence PNG/log/text file')
    fetch.add_argument('--name', required=True)
    fetch.add_argument('--out', type=Path, required=True)
    sub.add_parser('status', help='Server health, agents and overdue items')
    sub.add_parser('overdue', help='List overdue deliveries and requests')
    tracker = sub.add_parser('track', help='Show each recipient\'s state and times for a message')
    tracker.add_argument('--id', required=True)
    move = sub.add_parser('reassign', help='Give an unfinished REQUEST to another owner; notifies both')
    move.add_argument('--id', required=True)
    move.add_argument('--to', required=True)
    sub.add_parser('inbox', help='Print incoming messages once without archiving them')
    collector = sub.add_parser('collect', help='Archive incoming messages to Markdown and ACK owned requests')
    collector.add_argument('--no-ack', action='store_true', help='For migration of messages already acknowledged')
    collector.add_argument('--legacy-inbox', action='store_true',
                           help='Also drain the protocol 3 inbox of this host (mac/pc) once, with its old history')
    sub.add_parser('pending', help='List messages this agent has not marked done')
    done = sub.add_parser('done', help='Mark a message done after its work/result is complete')
    which = done.add_mutually_exclusive_group(required=True)
    which.add_argument('--id')
    which.add_argument('--seq', type=int)
    locker = sub.add_parser('lock', help='Acquire or release a shared-resource lock (pi-deploy, ksp-gamedata, '
                                         'board-flash, push-main) before using that resource')
    locker.add_argument('action', choices=('acquire', 'release'))
    locker.add_argument('name')
    locker.add_argument('--ttl', type=int, default=1800, help='Seconds until it expires by itself (60..14400)')
    locker.add_argument('--reason', default='')
    sub.add_parser('locks', help='List held locks with holder, reason and expiry')
    watcher = sub.add_parser('watch')
    watcher.add_argument('--once', action='store_true',
                         help='Exit when a message or a tracked change arrives; re-arm after handling it')
    args = parser.parse_args(argv)
    config_path = Path(args.config)
    if args.command == 'init':
        if config_path.exists():
            parser.error('configuration already exists; use another path instead of rotating a live token')
        if not valid_identity(args.peer):
            parser.error('identity must be HOST-AGENT such as pc-codex (bare mac, pc, pi are legacy)')
        parsed = urllib.parse.urlsplit(args.url)
        if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.query or parsed.fragment:
            parser.error('expected an HTTP(S) server URL without credentials or query')
        write_json(config_path, {'url': args.url.rstrip('/'), 'peer': args.peer,
                                 'token': args.token or secrets.token_urlsafe(32)})
        print('Created ' + str(config_path) + ' (private; do not commit)')
        return
    config = json.loads(config_path.read_text(encoding='utf-8'))
    if args.command == 'serve':
        journal = Journal(args.journal, args.allowlist)
        server = make_server(args.bind, args.port, journal, config['token'], args.artifacts)
        print('MAILBOX protocol %d listening on %s:%s; journal %s; register gate %s' % (
            PROTOCOL, *server.server_address, args.journal, journal.gate_summary()), flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()
    elif args.command == 'register':
        print(json.dumps(register(config, config_path, args.tool, args.takeover, args.release)))
    elif args.command == 'agents':
        listing = request(config, '/agents' + ('?retired=1' if args.retired else ''))
        print('MAILBOX register gate: %s' % listing.get('register_gate', 'unknown (older server)'))
        for agent in listing['agents']:
            print('AGENT {identity} {state} tool={tool} cwd={cwd} last={last}{retired}'.format(
                last=iso(agent['last_seen']), **dict(agent, retired=' RETIRED %s by %s' % (
                    iso(agent['retired']), agent.get('retired_by', '?')) if agent.get('retired') else '')))
    elif args.command == 'retire':
        agent = agent_request(config, config_path, '/retire', dict(target=args.identity, undo=args.undo))
        print('%s %s (state %s)' % (agent['identity'], 'retired' if agent.get('retired') else 'not retired',
                                    agent['state']))
    elif args.command == 'lock':
        result = legacy_request(config, '/locks', dict(action=args.action, name=args.name, holder=config['peer'],
                                                       ttl=args.ttl, reason=args.reason))
        print(json.dumps(result))
    elif args.command == 'locks':
        for lock in request(config, '/locks')['locks']:
            print('LOCK {name} held by {holder} until {until}: {reason}'.format(until=iso(lock['expires']), **lock))
    elif args.command == 'send':
        body = args.text if args.text is not None else args.file.read_text(encoding='utf-8')
        deadline = None if args.deadline is None else time.time() + 60 * args.deadline
        item = send_message(config, config_path, args.to, body, args.owner, deadline, args.id, args.legacy)
        to = item.get('to', '?')
        print('Stored message #%s (%s) for %s; receipt is not an acknowledgement: use track --id' % (
            item['seq'], item['id'], ','.join(to) if isinstance(to, list) else to))
    elif args.command == 'fetch':
        payload = request_bytes(config, '/artifact?' + urllib.parse.urlencode({'name': args.name}))
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(payload)
        print('Saved evidence to ' + str(args.out))
    elif args.command == 'status':
        health = request(config, '/health')
        print(json.dumps(health))
        print('MAILBOX register gate: %s' % health.get('register_gate', 'unknown (older server)'))
        for line in show_overdue(request(config, '/overdue')['overdue']):
            print(line)
    elif args.command == 'overdue':
        items = request(config, '/overdue')['overdue']
        print('MAILBOX: %d overdue item(s)' % len(items))
        for line in show_overdue(items):
            print(line)
    elif args.command == 'track':
        try:
            print(show_track(request(config, '/track?' + urllib.parse.urlencode({'id': args.id}))))
        except urllib.error.HTTPError as error:
            raise refusal(error)
    elif args.command == 'reassign':
        print(show_track(agent_request(config, config_path, '/reassign', dict(id=args.id, to=args.to))))
    elif args.command == 'collect':
        collect(config, config_path, acknowledge=not args.no_ack, legacy_inbox=args.legacy_inbox)
    elif args.command == 'pending':
        pending = pending_messages(config, config_path)
        print('MAILBOX: %d pending incoming message(s)' % len(pending))
        for item in pending:
            print('PENDING #{seq} {id} {sender} -> {recipient} [{time}]\n{text}'.format(
                **dict(item, recipient=recipient_of(item))), flush=True)
    elif args.command == 'done':
        for item in mark_done(config, config_path, args.id, args.seq):
            print('Marked message #%d (%s) done' % (item['seq'], item['id']))
    else:
        watch(config, config_path, once=getattr(args, 'once', False), continuous=args.command == 'watch')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
    except urllib.error.HTTPError as error:
        print('MAILBOX error: ' + str(refusal(error) if error.code < 500 else error), file=sys.stderr)
        sys.exit(1)
    except (OSError, ValueError) as error:
        print('MAILBOX error: ' + str(error), file=sys.stderr)
        sys.exit(1)
