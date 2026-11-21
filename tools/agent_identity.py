#!/usr/bin/env python3
"""Identity resolution for the agent mailbox bus: (host, repo, scope) -> identity (stdlib only, Python 3.9+).

* **host** comes from the per-machine file ``<state dir>/host`` (one word such as ``pc``), never the hostname.
* **repo** and **scope** come from the versioned ``.agent-bus.toml`` at the worktree top (the nearest directory
  holding ``.agent-bus.toml``; the walk stops at the first directory holding ``.git`` without one)::

     repo = "KSPKontroller"
     [[identity]]
     host = "pc"
     identity = "pc-claude"
     [[identity]]
     host = "pc"
     identity = "pc-codex"
     subdir = "tools"          # optional: prefix of the cwd relative to the worktree top
     branch = "agent/codex-*"  # optional: exact branch name or fnmatch pattern

  An entry matches when its host equals the host file and every matcher it has matches. The most specific
  match wins: deeper ``subdir`` first, then exact ``branch`` over a pattern over none. A tie between different
  identities is an error and resolves to no identity.
* ``AGENT_IDENTITY`` (one identity or a comma list, for multi-identity sessions) overrides the file.
* **state dir**: ``AGENT_BUS_STATE``, else ``%APPDATA%\\agent-bus`` on Windows, else ``~/.config/agent-bus``. It holds
  ``host``, ``<identity>.json`` with its per-identity files and ``<identity>.home.json`` (written by ``claim``).
  The client looks there first; ``build/agent-mailbox/`` stays the fallback, so existing configs keep working.
"""
import fnmatch
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys

TOML = '.agent-bus.toml'
HOST_FILE = 'host'
HOST_NAME = re.compile(r'^[a-z0-9][a-z0-9-]{0,19}$')
IDENTITY = re.compile(r'^[a-z0-9]{1,20}-[a-z0-9]{1,20}$')
FALLBACK = Path('build') / 'agent-mailbox'


class ResolutionError(ValueError):
    pass


# -- minimal TOML (the subset .agent-bus.toml uses) ----------------------------------------------------------------
def _value(text, where):
    text = text.strip()
    if text[:1] in ('"', "'"):
        quote, out, i = text[0], [], 1
        while i < len(text):
            c = text[i]
            if c == quote:
                rest = text[i + 1:].strip()
                if rest and not rest.startswith('#'):
                    raise ResolutionError('%s: unexpected text after string' % where)
                return ''.join(out)
            if c == '\\' and quote == '"':
                i += 1
                esc = text[i:i + 1]
                mapped = {'"': '"', '\\': '\\', 'n': '\n', 't': '\t'}.get(esc)
                if mapped is None:
                    raise ResolutionError('%s: unsupported escape \\%s' % (where, esc))
                out.append(mapped)
            else:
                out.append(c)
            i += 1
        raise ResolutionError('%s: unterminated string' % where)
    text = text.split('#', 1)[0].strip()
    if text in ('true', 'false'):
        return text == 'true'
    if re.match(r'^[+-]?\d+$', text):
        return int(text)
    raise ResolutionError('%s: unsupported value %r (strings, integers and booleans only)' % (where, text))


def parse_toml_subset(text):
    """Top-level ``key = value``, ``[table]`` and ``[[array]]`` tables; values are strings, integers, booleans."""
    data, current = {}, None
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        where = 'line %d' % number
        if not line or line.startswith('#'):
            continue
        m = re.match(r'^\[\[\s*([A-Za-z0-9_-]+)\s*\]\]\s*(#.*)?$', line)
        if m:
            current = {}
            data.setdefault(m.group(1), []).append(current)
            continue
        m = re.match(r'^\[\s*([A-Za-z0-9_-]+)\s*\]\s*(#.*)?$', line)
        if m:
            current = data.setdefault(m.group(1), {})
            continue
        m = re.match(r'^([A-Za-z0-9_-]+)\s*=\s*(.+)$', line)
        if not m:
            raise ResolutionError('%s: cannot parse %r' % (where, line))
        target = data if current is None else current
        if m.group(1) in target:
            raise ResolutionError('%s: duplicate key %s' % (where, m.group(1)))
        target[m.group(1)] = _value(m.group(2), where)
    return data


def parse_toml(text):
    try:
        import tomllib  # Python 3.11+
    except ImportError:
        return parse_toml_subset(text)
    try:
        return tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ResolutionError(str(e))


# -- state dir and host --------------------------------------------------------------------------------------------
def state_dir(env=None, system=None):
    env = os.environ if env is None else env
    if env.get('AGENT_BUS_STATE'):
        return Path(os.path.expanduser(env['AGENT_BUS_STATE']))
    system = system or sys.platform
    if system.startswith('win'):
        base = env.get('APPDATA') or str(Path.home() / 'AppData' / 'Roaming')
        return Path(base) / 'agent-bus'
    return Path.home() / '.config' / 'agent-bus'


def read_host(state):
    """The host word from <state>/host (first non-comment line), None when the file is missing."""
    try:
        text = (Path(state) / HOST_FILE).read_text(encoding='utf-8-sig')
    except FileNotFoundError:
        return None
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            if not HOST_NAME.match(line):
                raise ResolutionError('%s: invalid host %r' % (Path(state) / HOST_FILE, line))
            return line
    raise ResolutionError('%s is empty' % (Path(state) / HOST_FILE))


# -- repository ------------------------------------------------------------------------------------------------------
def find_top(cwd):
    """(directory holding .agent-bus.toml, toml path) or (git top without one, None) or (None, None)."""
    here = Path(cwd).resolve()
    for d in [here] + list(here.parents):
        if (d / TOML).is_file():
            return d, d / TOML
        if (d / '.git').exists():
            return d, None
    return None, None


def git_dir(top):
    dot = Path(top) / '.git'
    if dot.is_dir():
        return dot
    try:
        text = dot.read_text(encoding='utf-8').strip()
    except OSError:
        return None
    if text.startswith('gitdir:'):
        g = Path(text[len('gitdir:'):].strip())
        return g if g.is_absolute() else (Path(top) / g).resolve()
    return None


def git_head(top):
    """(branch or None when detached, commit sha or None) read from the files; no git binary needed."""
    g = git_dir(top) if top else None
    if g is None:
        return None, None
    try:
        head = (g / 'HEAD').read_text(encoding='utf-8').strip()
    except OSError:
        return None, None
    if not head.startswith('ref:'):
        return None, head or None
    ref = head[4:].strip()
    branch = ref[len('refs/heads/'):] if ref.startswith('refs/heads/') else ref
    common = g
    try:
        c = (g / 'commondir').read_text(encoding='utf-8').strip()
        common = Path(c) if Path(c).is_absolute() else (g / c).resolve()
    except OSError:
        pass
    for base in (g, common):
        try:
            return branch, (base / ref).read_text(encoding='utf-8').strip()
        except OSError:
            pass
    try:
        for line in (common / 'packed-refs').read_text(encoding='utf-8').splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1] == ref:
                return branch, parts[0]
    except OSError:
        pass
    return branch, None


def relative_scope(top, cwd):
    try:
        rel = Path(cwd).resolve().relative_to(Path(top).resolve())
    except ValueError:
        return ''
    return '' if str(rel) == '.' else rel.as_posix()


def _parts(path):
    return [p for p in PurePosixPath(str(path).replace('\\', '/')).parts if p not in ('', '.', '/')]


def specificity(entry, scope, branch):
    """None when the entry does not match the scope/branch, else a comparable score (subdir depth, branch kind)."""
    sub = entry.get('subdir')
    depth = 0
    if sub:
        want, have = _parts(sub), _parts(scope)
        if have[:len(want)] != want:
            return None
        depth = len(want)
    pattern = entry.get('branch')
    kind = 0
    if pattern:
        if branch is None:
            return None
        if pattern == branch:
            kind = 2
        elif any(c in pattern for c in '*?[') and fnmatch.fnmatchcase(branch, pattern):
            kind = 1
        else:
            return None
    return depth, kind


def match(entries, host, scope='', branch=None):
    """The identity of the most specific entry for this host; ResolutionError on a tie or no match."""
    scored = []
    for n, e in enumerate(entries):
        if not isinstance(e, dict) or not isinstance(e.get('identity'), str) or not isinstance(e.get('host'), str):
            raise ResolutionError('[[identity]] #%d needs host and identity strings' % (n + 1))
        if e['host'] != host:
            continue
        s = specificity(e, scope, branch)
        if s is not None:
            scored.append((s, e['identity']))
    if not scored:
        raise ResolutionError('no [[identity]] for host %s, scope %r, branch %s' % (host, scope or '.', branch))
    best = max(s for s, _ in scored)
    names = sorted({i for s, i in scored if s == best})
    if len(names) > 1:
        raise ResolutionError('tie between %s (host %s, scope %r, branch %s): make one entry more specific'
                              % (', '.join(names), host, scope or '.', branch))
    return names[0]


def split_identities(text):
    names = [n.strip() for n in text.split(',') if n.strip()]
    bad = [n for n in names if not IDENTITY.match(n)]
    if bad or not names:
        raise ResolutionError('AGENT_IDENTITY: invalid identity %s' % (', '.join(bad) or repr(text)))
    return list(dict.fromkeys(names))


def resolve(cwd=None, env=None, system=None):
    """Everything a session needs to know about itself. ``identities`` is empty and ``error`` set when the
    identity cannot be resolved; the repo context (worktree, repo, scope, branch, head) is filled when known."""
    env = os.environ if env is None else env
    cwd = Path(cwd or os.getcwd())
    state = state_dir(env, system)
    out = dict(identities=[], source=None, error=None, host=None, state_dir=str(state), worktree=None, toml=None,
               repo=None, scope='', branch=None, head=None, cwd=str(cwd))
    top, toml = find_top(cwd)
    entries = []
    if top is not None:
        out['worktree'] = str(top)
        out['scope'] = relative_scope(top, cwd)
        out['branch'], out['head'] = git_head(top)
    try:
        out['host'] = read_host(state)
        if toml is not None:
            out['toml'] = str(toml)
            data = parse_toml(toml.read_text(encoding='utf-8-sig'))
            out['repo'] = data.get('repo') if isinstance(data.get('repo'), str) else None
            entries = data.get('identity', [])
            if not isinstance(entries, list):
                raise ResolutionError('%s: identity must be an array of tables ([[identity]])' % toml)
        if env.get('AGENT_IDENTITY'):
            out['identities'], out['source'] = split_identities(env['AGENT_IDENTITY']), 'AGENT_IDENTITY'
        elif toml is None:
            raise ResolutionError('no %s at the worktree top (%s) and no AGENT_IDENTITY' % (TOML, top or cwd))
        elif out['host'] is None:
            raise ResolutionError('no host file: write the host word (pc, mac, pi) to %s' % (state / HOST_FILE))
        else:
            out['identities'] = [match(entries, out['host'], out['scope'], out['branch'])]
            out['source'] = str(toml)
    except ResolutionError as e:
        out['identities'], out['source'], out['error'] = [], None, str(e)
    return out


# -- per-identity files ----------------------------------------------------------------------------------------------
def config_candidates(identity, state=None, cwd=None, worktree=None, tools=None):
    """Where <identity>.json may live, in order: the state dir, then build/agent-mailbox/ of the cwd, the worktree
    top and the tools home (the checkout holding this script)."""
    state = Path(state) if state is not None else state_dir()
    tools = Path(tools) if tools is not None else Path(__file__).resolve().parents[1]
    out = [state / (identity + '.json')]
    for base in (Path(cwd or os.getcwd()), worktree, tools):
        if base is None:
            continue
        p = (Path(base) / FALLBACK / (identity + '.json')).absolute()
        if p not in out:
            out.append(p)
    return out


def config_path(identity, state=None, cwd=None, worktree=None, tools=None, exists=None):
    """The first existing candidate; the state dir's path when none exists yet."""
    candidates = config_candidates(identity, state, cwd, worktree, tools)
    exists = exists or (lambda p: Path(p).is_file())
    return next((p for p in candidates if exists(p)), candidates[0])


def home_path(config):
    """<identity>.home.json next to the identity's config (in the state dir, or the fallback folder)."""
    config = Path(config)
    return config.with_name(config.stem + '.home.json')


def read_home(config):
    try:
        data = json.loads(home_path(config).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


if __name__ == '__main__':
    print(json.dumps(resolve(), indent=2))
