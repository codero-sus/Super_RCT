#!/usr/bin/env python3
"""Update this checkout from its git remote.

    python3 tools/update.py            # check only: fetch and list what would change
    python3 tools/update.py --apply    # fast-forward to the upstream branch

The source is always origin/<current branch>, so the checkout follows the branch
it is on, whatever upstream git has configured.

Safety rules:
  * Only a fast-forward is applied. Local commits (diverged history) are refused.
  * A tree with uncommitted changes to tracked files is refused.
  * Untracked files (for example secret.env, build output) are never touched.
  * After --apply, the generated command tables are checked and the result is printed.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class UpdateError(Exception):
    pass


def git(root, *args, check=True):
    res = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)
    if check and res.returncode != 0:
        raise UpdateError(f"git {' '.join(args)} failed: {res.stderr.strip() or res.stdout.strip()}")
    return res


def source(root):
    """Fetch origin/<current branch> and return (branch, remote ref)."""
    branch = git(root, "rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
    if branch == "HEAD":
        raise UpdateError("the checkout is in detached HEAD state; check out the branch first.")
    res = git(root, "fetch", "--quiet", "origin", branch, check=False)
    if res.returncode != 0:
        if "couldn't find remote ref" in res.stderr:
            raise UpdateError(f"origin has no branch '{branch}'.")
        raise UpdateError(f"could not fetch origin/{branch}: {res.stderr.strip() or 'fetch failed'}")
    ref = f"origin/{branch}"
    if git(root, "rev-parse", "--verify", "--quiet", ref, check=False).returncode != 0:
        raise UpdateError(f"origin has no branch '{branch}'.")
    return branch, ref


def status(root):
    """Fetch the upstream and describe the difference. Changes nothing locally."""
    branch, up = source(root)
    behind = int(git(root, "rev-list", "--count", f"HEAD..{up}").stdout.strip())
    ahead = int(git(root, "rev-list", "--count", f"{up}..HEAD").stdout.strip())
    dirty = git(root, "status", "--porcelain", "--untracked-files=no").stdout.strip()
    changed = []
    if behind:
        out = git(root, "diff", "--name-status", f"HEAD..{up}").stdout
        changed = [line for line in out.splitlines() if line.strip()]
    return {"branch": branch, "source": up, "behind": behind, "ahead": ahead,
            "dirty": dirty, "changed": changed}


def report(info):
    lines = [f"branch:   {info['branch']}",
             f"source:   {info['source']}",
             f"behind:   {info['behind']} commit(s)",
             f"ahead:    {info['ahead']} commit(s)"]
    if info["changed"]:
        lines.append(f"files that would change ({len(info['changed'])}):")
        lines += [f"  {line}" for line in info["changed"][:200]]
        if len(info["changed"]) > 200:
            lines.append(f"  ... and {len(info['changed']) - 200} more")
    return "\n".join(lines)


def apply(root):
    info = status(root)
    print(report(info))
    if info["ahead"]:
        raise UpdateError(f"this checkout has local commits that are not on {info['source']}; "
                          "nothing was changed. Push or rebase them first.")
    if info["dirty"]:
        raise UpdateError("tracked files have uncommitted changes; nothing was changed. "
                          "Commit or stash them, then run the update again.")
    if not info["behind"]:
        return "Already up to date."
    git(root, "merge", "--ff-only", info["source"])
    check = subprocess.run([sys.executable, os.path.join(root, "tools", "gen_commands.py"), "--check"],
                           capture_output=True, text=True)
    note = check.stdout.strip() or check.stderr.strip()
    if check.returncode != 0:
        note += "\nThe generated command tables are stale. Run: python3 tools/gen_commands.py"
    return f"Updated to {info['source']}.\n{note}".rstrip()


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description="Update this checkout from its git remote.")
    parser.add_argument("--apply", action="store_true",
                        help="fast-forward to the upstream branch (default: check only)")
    args = parser.parse_args(argv)
    try:
        if args.apply:
            print(apply(root))
        else:
            print(report(status(root)))
            print("Run with --apply to update.")
    except UpdateError as exc:
        print(f"update: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
