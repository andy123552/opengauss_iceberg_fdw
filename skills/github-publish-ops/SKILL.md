---
name: github-publish-ops
description: Publish validated local changes to GitHub forks and create or update pull requests through a repeatable Windows GitHub CLI workflow. Use when Codex needs to push one or more repositories, recover from flaky remote-host GitHub access, update existing PRs, or batch-apply a shared PR template across several repositories.
---

# Github Publish Ops

## Overview

Use this skill when the code is ready and the remaining work is GitHub publication.
This skill is especially useful when development happens on a remote Linux host but reliable GitHub auth and push access live on the local Windows machine.

## Workflow

1. Confirm publish scope before staging or pushing.
   Inspect branch, diff, and worktree state in every target repository.

2. If GitHub access from the remote host is flaky, keep development and validation on the host but publish from Windows.
   Preferred fallback:
   - generate `git format-patch -1 --stdout HEAD` on the remote host
   - copy the patch to a clean Windows clone
   - replay it on top of the intended parent commit
   - push from Windows with `gh` and normal git auth

3. For DataInfraLab repositories, assume fork-based publication unless write access to upstream is explicitly confirmed.
   Push to `andy123552/<repo>` and open or update PRs against `DataInfraLab/<repo>`.

4. For existing PR branches:
   - fetch the fork branch first
   - update with `git push fork HEAD:refs/heads/<branch> --force-with-lease`
   This keeps the PR current without blind force pushes.

5. For new PRs:
   - push the branch to the fork
   - create the PR with Windows `gh pr create`
   - prefer a body file over long inline strings

6. For bulk PR edits:
   - materialize each body into a local markdown file
   - run `gh pr edit --body-file <file>` for each PR
   This is the most reliable way to apply a shared template.

7. After every publish step, verify:
   - pushed branch head
   - PR URL
   - PR title/body shape
   - relevant `gh pr checks` status when the task includes CI follow-up

## Guardrails

- Do not treat a local commit as completion when the user asked to submit, upload, or update a PR.
- Do not embed GitHub tokens in remote URLs or leave them behind in git config.
- Do not push unrelated worktree changes just because they are present in the same repository.
- Do not assume the remote host can authenticate to GitHub reliably; prefer Windows `gh` once the publish path gets shaky.

## Known Good Pattern

For the phase1 maintenance split, a reliable multi-repo publish flow was:
- develop and validate under `/data/ad/stack/data_infra`
- export one patch per repository from the remote host
- replay patches into clean Windows clones
- push to `andy123552` fork branches
- update existing PRs with `gh pr edit`
- create missing PRs with `gh pr create`
