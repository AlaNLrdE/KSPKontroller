#!/usr/bin/env python3
"""Merge an agent's worktree branch into the current branch, then run the static checks.

    build/venv/Scripts/python.exe tools/merge_agent.py <branch>

Stops with exit code 1 on a merge conflict (resolve by hand, `git add`, `git commit`, then run
`tools/verify.py`) or when the working tree has uncommitted changes. Never pushes, deploys or flashes.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args):
    p = subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    branch = sys.argv[1]
    rc, out = git("status", "--porcelain", "--untracked-files=no")
    if out:
        print("working tree has uncommitted changes: commit or stash first\n" + out)
        return 1
    rc, out = git("merge", "--no-ff", branch, "-m", "Merge " + branch)
    if rc != 0:
        rc2, conflicts = git("diff", "--name-only", "--diff-filter=U")
        print("merge stopped; conflicting files:\n" + (conflicts or out))
        return 1
    print("merged: " + git("log", "--oneline", "-1")[1])
    # CHANGELOG and AssemblyInfo must still agree after two branches bumped the version
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "verify.py")], capture_output=True, text=True)
    lines = [l for l in p.stdout.splitlines() if l.startswith(("FAIL", "WARN")) or "checks:" in l]
    print("\n".join(lines))
    return 0 if p.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
