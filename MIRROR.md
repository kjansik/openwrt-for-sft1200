# How this mirror works

- Canonical: <https://codeberg.org/kjansik/openwrt-for-sft1200> (Forgejo on Codeberg)
- This repository: read-only mirror on GitHub

## What is here, what is not

| | Codeberg (canonical) | GitHub (this mirror) |
|---|---|---|
| branches | `main` (upstream snapshot), `sync` (build) | `github-mirror` only |
| tags | yes | same tags, same commits |
| CI | `.forgejo/workflows/*` | `.github/workflows/sft1200-consistency.yml` |
| issues, pull requests | yes | no — report on Codeberg |
| releases | canonical | convenience copy of the same notes |

`main` and `sync` are deliberately not mirrored as branches:

- `main` is an upstream snapshot, not the tree the images are built from. As a
  mirrored default branch it would put stale code on the front page.
- pushing `sync` verbatim would also push the upstream OpenWrt workflows in
  `.github/workflows/` (11 of them, targeting upstream's buildbot, coverity,
  container publishing and an LLM review that needs an API key). Forgejo never
  runs them because `.forgejo/workflows/` takes precedence; GitHub would run
  every one of them on push.

So the mirror branch carries the port's content plus a trimmed `.github/`.

## Refreshing the mirror

    git switch github-mirror
    git merge --no-edit sync      # brings new build commits
    git push github github-mirror
    git push github --tags        # only when a release tag was added

`git merge sync` is expected to stay conflict-free: the branch edits only
`README.md`, `MIRROR.md` and `.github/`. If `sync` changes `README.md` (it is the
upstream OpenWrt readme there, with a port banner on top), the conflict is on
this file pair only — keep the mirror version.

## What differs from the Codeberg tree

- `.github/` is trimmed: upstream workflows, issue templates for
  bugs.openwrt.org, the labeler, `formalities.json`, `llm-review-rules.md`,
  the pull request template and the funding file are removed. They describe
  upstream OpenWrt's process, not this port.
- `.github/workflows/sft1200-consistency.yml` is the Codeberg consistency job
  ported to `ubuntu-latest`; it checks the same five things (image hashes,
  config packages, patched lines with their content, case collisions,
  `.gitattributes`).
- `.forgejo/workflows/` is kept for reference. GitHub ignores it; it is what
  runs on Codeberg.
- `README.md` is rewritten for GitHub. The Codeberg front page keeps the
  upstream OpenWrt README under a port banner.

Nothing else is edited on this branch.
