---
name: agent-identity
description: Resolving, claiming and releasing a mailbox identity by (host, repo, scope) with tools/agent_identity.py and agent_mailbox.py whoami/claim/unclaim, the per-machine state dir and the daemon home pointer. Use when a session starts or ends on the mailbox bus, when a config path is not found, or when changing identity resolution.
---

## Commands

- `agent_mailbox.py whoami`: prints the resolution (host file, `.agent-bus.toml`, scope, branch, head, config
  candidates, home.json). Sends nothing; exit 2 when nothing resolves. Run it first when unsure.
- `agent_mailbox.py claim`: session start. Exit 3 and no change when a live local session (home.json or the daemon
  pause names a running process) or a live lease elsewhere (409) holds the identity: report it, never force.
- `agent_mailbox.py unclaim`: session end; resumes the daemon, keeps the worktree in home.json.
- Without `--config` every command uses the resolved identity; `--identity NAME` picks one of a comma list.

## Facts and gotchas

- host comes from `<state dir>/host`, never the hostname. State dir: `AGENT_BUS_STATE`, `%APPDATA%\agent-bus`,
  `~/.config/agent-bus` (Pi: `~/dev/run/agent-mailbox`). `build/agent-mailbox/` is only the fallback.
- The toml search stops at the first directory with `.git`: a worktree created from a branch without
  `.agent-bus.toml` resolves nothing (use `AGENT_IDENTITY` or `--config`).
- A tie between two entries is an error by design: make one entry more specific (deeper `subdir`, exact `branch`).
- The session file is per config, so a dead local session and the daemon share it: the live-local-pid check in
  home.json and the pause holder is what protects a running session, not the server lease.
- Headless daemon runs set `AGENT_DAEMON_RUN=1`; claim/unclaim are no-ops there (a hook would otherwise pause the
  daemon that started the run).
- A home.json worktree that was deleted stops headless runs and sends one `INFO ... unhealthy` to all: claim again
  from an existing worktree.
- Tests: `IdentityResolutionTest` and the claim tests in `tools/test_agent_mailbox.py`, `HomePointerTest` in
  `tools/test_agent_daemon.py`. In the Bash tool, Python test strings with many backslashes get mangled in heredocs:
  write edit scripts with the Write tool and build backslashes with `chr(92)`.
