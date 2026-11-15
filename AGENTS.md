# KSPKontroller: instructions for Codex sessions and agents

The shared contract is `CLAUDE.md` at the repository root: read it first and follow it exactly.
Wherever it says "Claude session", read "Codex session". Skills live in `.claude/skills/*/SKILL.md`;
`.agents/skills/` is an exact copy for Codex: after editing a skill, copy it there.
Where `CLAUDE.md` names Claude Code tools (Agent, SendMessage, Monitor, background shells), use the Codex
equivalent; if there is none, do the step yourself or ask the user. Other sessions (`ksp-workbench`) are reached
only through the user: give the user a ready-to-paste message.
The latest session handoff (what is deployed, what is pending) is in the newest commit messages: `git log -10`.
Keep this file a pointer: add rules and lessons to `CLAUDE.md`, never here.
