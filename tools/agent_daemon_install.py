#!/usr/bin/env python3
"""Install or uninstall the mailbox listener (tools/agent_daemon.py) as a native per-user service.

One service per agent identity, run from that agent's own repo with the identity's own mailbox config:

* Linux / Pi: systemd user unit ``agent-daemon@<identity>.service`` (tools/systemd/agent-daemon@.service; the
  daemon is copied to ~/.local/lib/agent-daemon/ so a worktree change cannot alter a running listener).
* macOS: LaunchAgent ``~/Library/LaunchAgents/com.kspkontroller.agent-daemon.<identity>.plist`` (RunAtLoad,
  KeepAlive on failure), loaded with ``launchctl bootstrap gui/<uid>``.
* Windows: Task Scheduler task ``\\KSPKontroller\\agent-daemon-<identity>`` (at logon, restart on failure, no time
  limit) running the repo's build/venv pythonw.exe, registered with ``schtasks /Create /XML``.

Always look first::

   python3 tools/agent_daemon_install.py --identity pi-claude install --dry-run
   build/venv/Scripts/python.exe tools/agent_daemon_install.py --identity pc-claude install --dry-run
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


def plan_linux(identity, repo, home, max_runs=6, **_):
    """systemd --user: the unit template stays the source; only a repo outside ~/wk/KSPKontroller is rewritten."""
    home, repo = PurePosixPath(home), PurePosixPath(repo)
    unit = UNIT_TEMPLATE.read_text(encoding='utf-8').replace('\r\n', '\n')
    if repo != home / 'wk' / 'KSPKontroller':
        unit = unit.replace('%h/wk/KSPKontroller', str(repo))
    if max_runs != 6:
        unit = unit.replace('run --max-runs 6', 'run --max-runs %d' % max_runs)
    service = 'agent-daemon@%s.service' % identity
    return dict(
        files=[(str(home / '.local/lib/agent-daemon/agent_daemon.py'), ('copy', str(repo / 'tools/agent_daemon.py'))),
               (str(home / '.config/systemd/user/agent-daemon@.service'), unit)],
        install=[['systemctl', '--user', 'daemon-reload'], ['systemctl', '--user', 'enable', '--now', service]],
        uninstall=[['systemctl', '--user', 'disable', '--now', service]],
        remove=[],  # the template unit and the copied daemon may serve other identities on this host
        notes=['needs lingering for a start at boot without login: loginctl enable-linger (the user runs it)'])


def plan_macos(identity, repo, home, uid, max_runs=6, python='/usr/bin/python3', **_):
    home, repo = PurePosixPath(home), PurePosixPath(repo)
    base = repo / 'build/agent-mailbox' / (identity + '.daemon')
    plist = plistlib.dumps({
        'Label': label(identity),
        'ProgramArguments': [python, str(repo / 'tools/agent_daemon.py'), '--config',
                             str(repo / 'build/agent-mailbox' / (identity + '.json')), 'run', '--max-runs',
                             str(max_runs)],
        'WorkingDirectory': str(repo),
        'EnvironmentVariables': {'AGENT_DAEMON_REPO': str(repo),
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
                remove=[path], notes=['logs: ' + str(base / 'daemon.log')])


def task_xml(identity, repo, user, max_runs=6):
    repo = PureWindowsPath(repo)
    pythonw = repo / 'build' / 'venv' / 'Scripts' / 'pythonw.exe'
    base = repo / 'build' / 'agent-mailbox' / (identity + '.daemon')
    arguments = '"%s" --config "%s" run --max-runs %d --log "%s"' % (
        repo / 'tools' / 'agent_daemon.py', repo / 'build' / 'agent-mailbox' / (identity + '.json'), max_runs,
        base / 'daemon.log')
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
      <WorkingDirectory>{repo}</WorkingDirectory>
    </Exec>
  </Actions>
</Task>
""".format(identity=escape(identity), uri=escape(task_name(identity)), user=escape(user),
           command=escape(str(pythonw)), arguments=escape(arguments), repo=escape(str(repo)))


def plan_windows(identity, repo, user, max_runs=6, **_):
    repo_w = PureWindowsPath(repo)
    xml_path = str(repo_w / 'build' / 'agent-mailbox' / (identity + '.daemon') / 'task.xml')
    name = task_name(identity)
    return dict(files=[(xml_path, ('utf-16', task_xml(identity, repo, user, max_runs)))],
                install=[['schtasks', '/Create', '/TN', name, '/XML', xml_path, '/F'], ['schtasks', '/Run', '/TN', name]],
                uninstall=[['schtasks', '/End', '/TN', name], ['schtasks', '/Delete', '/TN', name, '/F']],
                remove=[xml_path],
                notes=['runs %s; logs in %s' % (repo_w / 'build/venv/Scripts/pythonw.exe',
                                               repo_w / 'build/agent-mailbox' / (identity + '.daemon') / 'daemon.log')])


PLANS = {'linux': plan_linux, 'macos': plan_macos, 'windows': plan_windows}


def make_plan(system, identity, repo, home, uid=None, user=None, max_runs=6):
    return PLANS[system](identity=identity, repo=repo, home=home, uid=uid, user=user, max_runs=max_runs)


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
    ap.add_argument('--identity', required=True, help='HOST-AGENT, e.g. pi-claude; selects build/agent-mailbox/<id>.json')
    ap.add_argument('--os', choices=sorted(PLANS), help='default: this machine; another OS is dry-run only')
    ap.add_argument('--repo', help='the repo the daemon runs in (default: this checkout)')
    ap.add_argument('--max-runs', type=int, default=6)
    ap.add_argument('action', choices=('install', 'uninstall'))
    ap.add_argument('--dry-run', action='store_true', help='print what would be written and run, change nothing')
    args = ap.parse_args(argv)
    here = platform_name()
    system = args.os or here
    if system != here and not args.dry_run:
        ap.error('a real %s for %s must run on that OS; use --dry-run to review it here' % (args.action, system))
    repo = args.repo or str(HERE.parent)
    if system != here:
        repo = args.repo or (r'C:\Users\%s\wk\KSPKontroller' % getpass.getuser() if system == 'windows'
                             else '/home/%s/wk/KSPKontroller' % getpass.getuser() if system == 'linux'
                             else '/Users/%s/wk/KSPKontroller' % getpass.getuser())
    home = str(Path.home()) if system == here else str(PurePosixPath(repo).parents[1]) if system != 'windows' else ''
    uid = os.getuid() if hasattr(os, 'getuid') and system == here else 501
    user = os.environ.get('USERDOMAIN', '') + '\\' + getpass.getuser() if system == 'windows' else getpass.getuser()
    plan = make_plan(system, args.identity, repo, home, uid=uid, user=user, max_runs=args.max_runs)
    if args.dry_run:
        print('%s %s for %s (dry run, nothing changed)' % (system, args.action, args.identity))
        print(describe(plan, args.action))
        return 0
    return apply(plan, args.action)


if __name__ == '__main__':
    sys.exit(main())
