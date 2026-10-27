---
name: commit
description: >
  Clean one change, then commit only that change.
  Use when the user asks to commit, split a commit, or runs /commit.
---

# Commit

Follow the Git section in `AGENTS.md`.

## Clean the change

Read the diff before staging. Tidy that change and leave every other line alone.

- The diff does the work once, at the cause. Remove a debug log, a probe, or a commented-out block that this change added.
- Names match the surrounding file. A test that belongs to the change stays with it.
- After a header change or a C++ signature change, rebuild the way `AGENTS.md` says, and commit the tree you built.
- Do not restyle code outside the change. If the change is already clean, do not invent an edit.

## Split and commit

1. Read `git status`, `git diff`, and `git log` in this repository.
2. Group hunks by change. When one file holds two changes, set the other change aside, commit the first, then restore the rest.
3. Stage only that change and commit it. Repeat.
4. A Kindle binary that belongs to one source change goes in that commit, after the rebuild. When several source changes share one binary, commit the sources first, rebuild, and commit `dist/kindle/yomu/yomu` alone with subject `Refresh the Kindle binary.`
5. Leave unrelated dirty files unstaged.

Show each new subject and the repository it is in.
