#!/usr/bin/env python3
"""Install or uninstall the mailbox listener (tools/agent_daemon.py) as a native per-user service.

One service per agent identity. Two places are kept apart:

* the **tools home** (``--tools-dir``, default the KSPKontroller checkout containing this script): Python (the
  venv on Windows), tools/agent_daemon.py, tools/agent_mailbox.py and usually build/agent-mailbox/<identity>.json;
* the **work repo** (``--repo``/``--workdir``, default the tools home): the service's working directory, the
  headless ``claude -p`` cwd and the root of its Edit and ``git -C`` rules (``foreign`` profile when it differs).

``--config`` is resolved in this order: the option, ``<tools home>/build/agent-mailbox/<identity>.json``, then
``<work repo>/build/agent-mailbox/<identity>.json``; the dry run shows which one was used. A real install refuses
when the python, the daemon, the mailbox client, the config or the work repo is missing.

* Linux / Pi: systemd user unit ``agent-daemon@<identity>.service`` (tools/systemd/agent-daemon@.service; the
  daemon is copied to ~/.local/lib/agent-daemon/ so a worktree change cannot alter a running listener). A work
  repo, config or python of its own goes into the identity's drop-in ``agent-daemon@<identity>.service.d/``.
* macOS: LaunchAgent ``~/Library/LaunchAgents/com.kspkontroller.agent-daemon.<identity>.plist`` (RunAtLoad,
  KeepAlive on failure), loaded with ``launchctl bootstrap gui/<uid>``.
* Windows: Task Scheduler task ``\\KSPKontroller\\agent-daemon-<identity>`` (at logon, restart on failure, no time
  limit) running the tools home's build/venv pythonw.exe, registered with ``schtasks /Create /XML``.

Always look first::

   python3 tools/agent_daemon_install.py --identity pi-claude install --dry-run
   build/venv/Scripts/python.exe tools/agent_daemon_install.py --identity pc-claude install --dry-run
   build/venv/Scripts/python.exe tools/agent_daemon_install.py --identity pc-workbench \\
       --repo C:/Users/alanl/wk/decompilation/ksp-workbench install --dry-run
   python3 tools/agent_daemon_install.py --os macos --identity mac-workbench --repo /Users/alan/wk/devKSP/ksp1/ksp-workbench \\
       --tools-dir /Users/alan/wk/devKSP/ksp1/KSPKontroller \\
       --config /Users/alan/wk/devKSP/run/agent-mailbox/mac-workbench.json install --dry-run
   python3 tools/agent_daemon_install.py --identity mac-claude uninstall --dry-run

``--os`` renders another platform's service (for review); a real install only runs for the current platform.
"""

import argparse
import getpass
import os
from pathlib import Path, PureWindowsPath, PurePosixPath
import plistlib
import shutil
import subprocess
import sys
from xml.sax.saxutils import escape

HERE = Path(__file__).resolve().parent
UNIT_TEMPLATE = HERE / 'systemd' / 'agent-daemon@.service'
TASK_FOLDER = 'KSPKontroller'
DEFAULT_REPO = '~/wk/KSPKontroller'


def platform_name(value=None):
    value = value or sys.platform
    if value in ('windows', 'macos', 'linux'):
        return value
    return 'windows' if value.startswith('win') else 'macos' if value == 'darwin' else 'linux'


def label(identity):
    return 'com.kspkontroller.agent-daemon.' + identity


def task_name(identity):
    return '\\%s\\agent-daemon-%s' % (TASK_FOLDER, identity)


def pure(system):
    return PureWindowsPath if system == 'windows' else PurePosixPath


def config_candidates(system, identity, tools, work):
    p = pure(system)
    seen, out = set(), []
    for where, base in (('tools home', tools), ('work repo', work)):
        path = p(base) / 'build' / 'agent-mailbox' / (identity + '.json')
        if path not in seen:
            seen.add(path)
            out.append((where, path))
    return out


def resolve_config(system, identity, tools, work, config=None, exists=os.path.exists):
    """(path, source): the option, then the tools home, then the work repo; the tools home's path when none exists
    (the install then refuses, a dry run says so)."""
    if config:
        return pure(system)(config), '--config'
    candidates = config_candidates(system, identity, tools, work)
    for where, path in candidates:
        if exists(str(path)):
            return path, where
    return candidates[0][1], 'none found (default: tools home)'


def daemon_args(config, work, tools, max_runs, system):
    """agent_daemon.py arguments after the script: the work repo is explicit whenever it is not the tools home."""
    args = ['--config', str(config)]
    if pure(system)(work) != pure(system)(tools):
        args += ['--repo', str(work)]
    return args + ['run', '--max-runs', str(max_runs)]


def requirements(python, tools, config, work, system):
    p = pure(system)
    return [('python', str(python)), ('daemon', str(p(tools) / 'tools' / 'agent_daemon.py')),
            ('mailbox client', str(p(tools) / 'tools' / 'agent_mailbox.py')), ('config', str(config)),
            ('work repo', str(work))]


def plan_linux(identity, tools, work, config, home, max_runs=6, python=None, **_):
    """systemd --user: the unit template stays the source and the Pi keeps it unchanged (tools home = work repo =
    ~/dev/ksp1/KSPKontroller); another tools home rewrites the template, another work repo, config or python goes into
    the identity's own drop-in."""
    home, tools, work = PurePosixPath(home), PurePosixPath(tools), PurePosixPath(work)
    unit = UNIT_TEMPLATE.read_text(encoding='utf-8').replace('\r\n', '\n')
    if tools != home / 'dev' / 'ksp1' / 'KSPKontroller':
        unit = unit.replace('%h/dev/ksp1/KSPKontroller', str(tools))
    if max_runs != 6:
        unit = unit.replace('run --max-runs 6', 'run --max-runs %d' % max_runs)
    service = 'agent-daemon@%s.service' % identity
    dropin = str(home / '.config/systemd/user' / (service + '.d') / 'work-repo.conf')
    files = [(str(home / '.local/lib/agent-daemon/agent_daemon.py'), ('copy', str(tools / 'tools/agent_daemon.py'))),
             (str(home / '.config/systemd/user/agent-daemon@.service'), unit)]
    default_config = tools / 'build/agent-mailbox' / (identity + '.json')
    if work != tools or PurePosixPath(config) != default_config or python:
        lines = ['# %s: work repo %s, tools home %s (tools/agent_daemon_install.py)' % (identity, work, tools),
                 '[Service]', 'WorkingDirectory=%s' % work, 'Environment=AGENT_DAEMON_REPO=%s' % tools,
                 'Environment=AGENT_DAEMON_WORK_REPO=%s' % work, 'ExecStart=',
                 'ExecStart=%s %%h/.local/lib/agent-daemon/agent_daemon.py %s' % (
                     python or '/usr/bin/python3', ' '.join(daemon_args(config, work, tools, max_runs, 'linux')))]
        files.append((dropin, '\n'.join(lines) + '\n'))
    return dict(
        files=files,
        install=[['systemctl', '--user', 'daemon-reload'], ['systemctl', '--user', 'enable', '--now', service]],
        uninstall=[['systemctl', '--user', 'disable', '--now', service]],
        remove=[dropin],  # the template unit and the copied daemon may serve other identities on this host
        requires=requirements(python or '/usr/bin/python3', tools, config, work, 'linux'),
        notes=['needs lingering for a start at boot without login: loginctl enable-linger (the user runs it)'])


def plan_macos(identity, tools, work, config, home, uid, max_runs=6, python=None, **_):
    home, tools, work = PurePosixPath(home), PurePosixPath(tools), PurePosixPath(work)
    python = python or '/usr/bin/python3'
    base = PurePosixPath(config).parent / (PurePosixPath(config).stem + '.daemon')
    plist = plistlib.dumps({
        'Label': label(identity),
        'ProgramArguments': [python, str(tools / 'tools/agent_daemon.py')]
                            + daemon_args(config, work, tools, max_runs, 'macos'),
        'WorkingDirectory': str(work),
        'EnvironmentVariables': {'AGENT_DAEMON_REPO': str(tools), 'AGENT_DAEMON_WORK_REPO': str(work),
                                 'PATH': '%s/.local/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin' % home},
        'RunAtLoad': True,
        'KeepAlive': {'SuccessfulExit': False},
        'ThrottleInterval': 30,
        'ProcessType': 'Background',
        'StandardOutPath': str(base / 'daemon.log'),
        'StandardErrorPath': str(base / 'daemon.log'),
    }).decode('utf-8')
    path = str(home / 'Library/LaunchAgents' / (label(identity) + '.plist'))
    return dict(files=[(path, plist)],
                install=[['launchctl', 'bootstrap', 'gui/%s' % uid, path]],
                uninstall=[['launchctl', 'bootout', 'gui/%s/%s' % (uid, label(identity))]],
                remove=[path], requires=requirements(python, tools, config, work, 'macos'),
                notes=['logs: ' + str(base / 'daemon.log')])


def task_xml(identity, tools, work, config, user, max_runs=6, python=None):
    tools, work, config = PureWindowsPath(tools), PureWindowsPath(work), PureWindowsPath(config)
    pythonw = PureWindowsPath(python) if python else tools / 'build' / 'venv' / 'Scripts' / 'pythonw.exe'
    base = config.parent / (config.stem + '.daemon')
    arguments = ' '.join('"%s"' % a if (' ' in a or '\\' in a or '/' in a) else a for a in (
        [str(tools / 'tools' / 'agent_daemon.py')] + daemon_args(config, work, tools, max_runs, 'windows')
        + ['--log', str(base / 'daemon.log')]))
    return """<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Mailbox listener for {identity} (tools/agent_daemon.py; wakes Claude Code only for new work)</Description>
    <URI>{uri}</URI>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
      <UserId>{user}</UserId>
      <Delay>PT30S</Delay>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>{user}</UserId>
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>LeastPrivilege</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>7</Priority>
    <RestartOnFailure>
      <Interval>PT1M</Interval>
      <Count>999</Count>
    </RestartOnFailure>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>{command}</Command>
      <Arguments>{arguments}</Arguments>
      <WorkingDirectory>{work}</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
""".format(identity=escape(identity), uri=escape(task_name(identity)), user=escape(user),
           command=escape(str(pythonw)), arguments=escape(arguments), work=escape(str(work)))


def plan_windows(identity, tools, work, config, user, max_runs=6, python=None, **_):
    tools_w, config_w = PureWindowsPath(tools), PureWindowsPath(config)
    base = config_w.parent / (config_w.stem + '.daemon')
    xml_path = str(base / 'task.xml')
    pythonw = PureWindowsPath(python) if python else tools_w / 'build' / 'venv' / 'Scripts' / 'pythonw.exe'
    name = task_name(identity)
    return dict(files=[(xml_path, ('utf-16', task_xml(identity, tools, work, config, user, max_runs, python)))],
                install=[['schtasks', '/Create', '/TN', name, '/XML', xml_path, '/F'], ['schtasks', '/Run', '/TN', name]],
                uninstall=[['schtasks', '/End', '/TN', name], ['schtasks', '/Delete', '/TN', name, '/F']],
                remove=[xml_path], requires=requirements(pythonw, tools, config, work, 'windows'),
                notes=['runs %s in %s; logs in %s' % (pythonw, PureWindowsPath(work), base / 'daemon.log')])


PLANS = {'linux': plan_linux, 'macos': plan_macos, 'windows': plan_windows}


def make_plan(system, identity, tools, home, uid=None, user=None, max_runs=6, work=None, config=None, python=None):
    """``tools`` is the tools home; ``work`` (default: the tools home) the work repo; ``config`` defaults to the
    tools home's build/agent-mailbox/<identity>.json."""
    work = work or tools
    config = config or str(pure(system)(tools) / 'build' / 'agent-mailbox' / (identity + '.json'))
    return PLANS[system](identity=identity, tools=tools, work=work, config=config, home=home, uid=uid, user=user,
                         max_runs=max_runs, python=python)


def missing(plan, exists=os.path.exists):
    return [(what, path) for what, path in plan['requires'] if not exists(path)]


def describe(plan, action):
    lines = []
    if action == 'install':
        for path, content in plan['files']:
            if isinstance(content, tuple) and content[0] == 'copy':
                lines.append('copy %s -> %s' % (content[1], path))
            else:
                text = content[1] if isinstance(content, tuple) else content
                lines.append('write %s:' % path)
                lines.extend('    ' + l for l in text.splitlines())
    commands = plan['install'] if action == 'install' else plan['uninstall']
    lines.extend('run ' + ' '.join(('"%s"' % a) if ' ' in a else a for a in c) for c in commands)
    if action == 'uninstall':
        lines.extend('remove ' + p for p in plan['remove'])
    lines.extend('note: ' + n for n in plan['notes'])
    return '\n'.join(lines)


def apply(plan, action):
    if action == 'install':
        for path, content in plan['files']:
            target = Path(path)
            target.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(content, tuple) and content[0] == 'copy':
                shutil.copyfile(content[1], target)
            elif isinstance(content, tuple):
                target.write_text(content[1], encoding=content[0])
            else:
                target.write_text(content, encoding='utf-8', newline='\n')
    rc = 0
    for argv in plan['install'] if action == 'install' else plan['uninstall']:
        print('run ' + ' '.join(argv), flush=True)
        r = subprocess.run(argv)
        rc = rc or r.returncode
    if action == 'uninstall':
        for path in plan['remove']:
            try:
                Path(path).unlink()
            except FileNotFoundError:
                pass
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--identity', required=True, help='HOST-AGENT, e.g. pi-claude; selects <identity>.json')
    ap.add_argument('--os', choices=sorted(PLANS), help='default: this machine; another OS is dry-run only')
    ap.add_argument('--tools-dir', help='tools home: python, agent_daemon.py, agent_mailbox.py (default: this '
                    'checkout; for another --os ~/wk/KSPKontroller of this user, ~/dev/ksp1/KSPKontroller on Linux)')
    ap.add_argument('--repo', '--workdir', dest='repo', help='work repo: service cwd, claude -p cwd and allowlist '
                    'root (default: the tools home)')
    ap.add_argument('--config', help='mailbox config (default: tools home, then work repo build/agent-mailbox/'
                    '<identity>.json)')
    ap.add_argument('--python', help='interpreter the service runs (default: tools venv pythonw.exe on Windows, '
                    '/usr/bin/python3 elsewhere)')
    ap.add_argument('--max-runs', type=int, default=6)
    ap.add_argument('action', choices=('install', 'uninstall'))
    ap.add_argument('--dry-run', action='store_true', help='print what would be written and run, change nothing')
    args = ap.parse_args(argv)
    here = platform_name()
    system = args.os or here
    if system != here and not args.dry_run:
        ap.error('a real %s for %s must run on that OS; use --dry-run to review it here' % (args.action, system))
    tools = args.tools_dir or str(HERE.parent)
    if system != here and not args.tools_dir:
        tools = (r'C:\Users\%s\wk\KSPKontroller' % getpass.getuser() if system == 'windows'
                 else '/home/%s/dev/ksp1/KSPKontroller' % getpass.getuser() if system == 'linux'
                 else '/Users/%s/wk/KSPKontroller' % getpass.getuser())
    tools = str(pure(system)(tools))
    work = str(pure(system)(args.repo or tools))
    if system == here:
        config, source = resolve_config(system, args.identity, tools, work, args.config)
    else:  # another OS: nothing to look at here
        config, source = resolve_config(system, args.identity, tools, work, args.config, exists=lambda _: False)
        source = '--config' if args.config else 'default: tools home (not checked on another OS)'
    home = str(Path.home()) if system == here else str(PurePosixPath(tools).parents[1]) if system != 'windows' else ''
    uid = os.getuid() if hasattr(os, 'getuid') and system == here else 501
    user = os.environ.get('USERDOMAIN', '') + '\\' + getpass.getuser() if system == 'windows' else getpass.getuser()
    plan = make_plan(system, args.identity, tools, home, uid=uid, user=user, max_runs=args.max_runs, work=work,
                     config=str(config), python=args.python)
    absent = missing(plan) if system == here and args.action == 'install' else []
    if args.dry_run:
        print('%s %s for %s (dry run, nothing changed)' % (system, args.action, args.identity))
        print('tools home: %s\nwork repo:  %s\nconfig:     %s (%s)' % (tools, work, config, source))
        print(describe(plan, args.action))
        for what, path in absent:
            print('missing: %s %s (a real install would refuse)' % (what, path))
        return 0
    if absent:
        for what, path in absent:
            print('refusing to install: %s %s does not exist' % (what, path), file=sys.stderr)
        return 2
    return apply(plan, args.action)


if __name__ == '__main__':
    sys.exit(main())
