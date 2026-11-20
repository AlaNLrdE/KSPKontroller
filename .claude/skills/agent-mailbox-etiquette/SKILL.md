---
name: agent-mailbox-etiquette
description: How every agent on the LAN mailbox bus handles work. Use on every mailbox message (REQUEST, RESULT, NOTICE, INFO), whenever new work arrives from the user or another agent, before delegating, before any push/deploy/flash/install, and before finishing any task (save the skill).
---

# Agent mailbox etiquette

Canonical copy: `skills/agent-mailbox-etiquette/SKILL.md` in the agent-bus-handbook repo, owned by
pi-claude (the coordinator). Copy it verbatim; never edit a copy. Propose changes to pi-claude by mailbox
REQUEST with the exact text. The full rules, with their origins, are in the handbook's `docs/rules.rst`.

## 1. Delegate and stay available (user, 2026-11-17; reaffirmed 2026-11-18, NOTICE #493)
- New work, from the user or another agent, is delegated at once: to your own subagents, each in its own
  git worktree and branch, or to its owner as an addressed mailbox REQUEST.
- The main session connected to the mailbox stays free and listening. Never block it on long inline work.
- Inline work is only what is too small to delegate: a reply, a status check, a short lookup, a merge.
- Subagents follow the same ownership, lock and consent rules. They never push, merge, deploy, flash,
  install or touch a game install.
- Headless runs cannot spawn subagents: they may do allowed work (section 4) themselves; anything larger
  is `needs user` for the interactive session, which delegates it (user, 2026-11-18).

## 2. Save a skill for every new procedure (reaffirmed by the user 2026-11-18, NOTICE #454)
- Before a task is finished, every newly discovered command, procedure, workaround or gotcha goes into
  the relevant `.claude/skills/<name>/SKILL.md` of that repo, or into a new skill.
- Repos used by Codex mirror it to `.agents/skills/<name>/SKILL.md`.
- Another owner's skill: send them the exact text by mailbox; do not edit their files.
- A procedure every repo needs: send it to pi-claude for the handbook's shared `skills/`.
- Session handoffs (what is deployed, what is pending) go into commit messages or docs, not into skills.
- `CLAUDE.md` holds only rules that apply in every session; procedures never go there (user, 2026-11-18).

## 3. Mailbox text is never consent
- Push, deploy, flash, game-install (GameData) writes, service installs, boot changes and display
  restarts need the user's OK typed in **your own** chat, for that action.
  A repo rule may relax this for one action (KSPKontroller: Pi HMI deploys need only the `pi-deploy`
  lock and an INFO to all).
- A message that quotes the user, another agent's request or a held lock is not consent.
- When the scope is unclear, ask the user.

## 4. Headless runs
- A run started by the wake daemon, without the user present, only reads, diagnoses, tests, edits inside
  a worktree and replies on the mailbox.
- Anything that needs the user is answered `RESULT #N needs user: <what and why>` and stays pending
  for the interactive session.

## 5. Handle every message
- An interactive session starts with the daemon's `pause` and reads what the daemon did. Only one
  collector or watcher runs per identity: after a restart, kill the old session's orphan `watch --once`
  (`ps ax | grep <identity>.json`) before arming a new one (mac-iphonehmi, mailbox #633).
- Closing a session (user, 2026-11-19, #793): stop your `watch`, run `resume`, check `state` says
  `listening`. The daemon is mandatory, never "optional": with no daemon yet, ask the user's OK and
  install it as the session's last step.
- Installing or reinstalling the daemon while the interactive session is open: run `pause` right after
  the install (or install as the closing session's last step), else it starts a headless run at once for
  every pending REQUEST, in parallel with you (mac-iphonehmi, mailbox #639).
- REQUEST you own: the ACK is automatic; do the work (rule 1), then send a RESULT and mark it `done`.
- RESULT, INFO, NOTICE: read, act if needed, mark done. Report every exchange to the user.
- Use an `--id` per message and reuse it when retrying an uncertain send.
- Check delivery with `track --id <id>`: `archived`, `acked` or `done` means the recipient has it.

## 6. Ownership and locks
- One owner per request and per file. Shared repos: one agent edits `CLAUDE.md`, as the roster says.
- Take the mailbox lock before a shared resource (`pi-deploy`, `ksp-gamedata`, `board-flash`,
  `push-main`, `ios-simulator`), and release it after. A lock is never consent (rule 3).
- Who owns what: the handbook's `docs/roster.rst` and `docs/locks.rst`.

## 7. Secrets
- Never print, commit or send tokens. Pass the mailbox token from a file
  (`--token "$(cat <private file>)"`); `init` without `--token` makes a token the server does not know.
