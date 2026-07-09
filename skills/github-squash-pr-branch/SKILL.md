---
name: github-squash-pr-branch
description: Squash multiple commits on an existing GitHub pull request branch into one clean commit and force-update the fork branch safely. Use when Codex needs to merge 2 or more commits on a PR branch into a single commit, keep the PR URL unchanged, and verify the remote PR head after the rewrite.
---

# GitHub Squash PR Branch

## Overview

Use this skill when the user wants an existing PR branch to keep the same branch name and PR,
but reduce multiple commits to one clean commit.

This project's default target is the physical machine `opengauss-ad`, using the repository's
fork remote and GitHub CLI there.

## Workflow

1. Confirm the target repository, branch, PR number, and the exact commit range to squash.
   Read:
   - `git branch --show-current`
   - `git log --oneline --decorate -5`
   - `gh pr view <pr> --repo <upstream/repo> --json headRefName,headRefOid,title,url`

2. Ensure the worktree is clean before rewriting history.
   If there are unrelated changes, stop and resolve them first.

3. Identify the base commit that should remain as the parent.
   For a branch with:
   - `base`
   - `commit A`
   - `commit B`
   squash by resetting softly to `base`.

4. Prefer a non-interactive squash:
   - `git reset --soft <base-commit>`
   - `git commit -m "<final message>"`

5. Push the rewritten branch safely:
   - `git push --force-with-lease fork <branch>`

6. Verify both the fork head and the PR head:
   - `git ls-remote fork refs/heads/<branch>`
   - `gh pr view <pr> --repo <upstream/repo> --json headRefOid,headRefName,url,title`

7. If GitHub metadata lags slightly behind the push, wait briefly and read the PR again.

## Physical Machine Notes

- On `opengauss-ad`, prefer GitHub CLI at:
  `/home/ad/.local/bin/gh`
- Do not assume `gh` is in `PATH` for non-interactive `ssh` commands.
- Before PR operations, check:
  - `command -v gh || true`
  - `ls /home/ad/.local/bin/gh`
  - `/home/ad/.local/bin/gh auth status`

## Guardrails

- Do not use interactive rebase unless the user explicitly wants it.
- Do not squash if the branch contains uncommitted work unrelated to the PR.
- Do not use plain `--force`; use `--force-with-lease`.
- Do not change the PR branch name when the goal is only to collapse commits.
- After pushing, do not assume GitHub has refreshed immediately; re-read the PR head SHA.

## Known Good Pattern

For a PR branch on `opengauss-ad` with two commits on top of base `18cde0c`:

1. `git reset --soft 18cde0c`
2. `git commit -m 'feat: expose phase1 maintenance bridge APIs'`
3. `git push --force-with-lease fork feat/gc-maintenance`
4. Verify:
   - local `git rev-parse HEAD`
   - remote `git ls-remote fork refs/heads/feat/gc-maintenance`
   - PR head via `gh pr view`
