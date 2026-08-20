---
name: release
description: Cut a Halo firmware release — version bump PR + CHANGELOG entry, CI build via build-and-release.yml, pre-release, hardware verification, tag, and the final GitHub release with renamed assets. Use when asked to release, tag, or publish firmware.
---

# Release Halo firmware

The whole process, in order. Versions are `MAJOR.MINOR.PATCH` with **no `v`
prefix** on the final tag/release (e.g. `0.8.8`). The `vX.Y.Z-<run>` tags are
CI **pre-releases** only — a separate namespace, left in place afterwards.

## 1. Version bump PR (with changelog)

On a fresh branch off `origin/main` (kebab-case, e.g. `chore-version-bump-0-8-9`):

- `applications/halo/VERSION`: bump `PATCHLEVEL` (or MINOR). `EXTRAVERSION`
  must stay **empty** — if the working tree shows `EXTRAVERSION = debug`,
  that's build noise; revert it, never commit it.
- `CHANGELOG.md`: move the release's content under a new
  `## [X.Y.Z] - YYYY-MM-DD` heading (Keep a Changelog categories: Added /
  Changed / Fixed / Removed / Documentation) and leave `## [Unreleased]`
  empty. Version headings are plain text, not links — releases up to 0.8.8
  live in the private archive, so there are no reference links to maintain.
  List every PR merged since the last tag:
  `git log --oneline <last-tag>..origin/main`.
- Commit as `Version Bump X.Y.Z`, open the PR, merge it as a **merge commit**
  (repo convention — never squash, or built SHAs stop being ancestors of main).

**Public-repo hygiene**: changelog and release notes are public. Describe what
the code does; no device-fault narratives, no private device names.

## 2. CI build + pre-release

```
gh workflow run build-and-release.yml -f branch=main -f create_release=true
gh run watch <run-id>   # ~15–25 min: two pristine sysbuild builds in Docker
```

The workflow builds **debug and release** variants pristine, uploads a
`halo-firmware-<ver>` artifact, and publishes pre-release `vX.Y.Z-<run#>` with:
`halo-firmware-X.Y.Z-{release,debug}.signed.bin` (OTA payloads),
`halo-bootloader-X.Y.Z-{release,debug}.bin` (wired factory flashing only),
and `VERSION.txt`. Known quirk: `VERSION.txt`'s `Commit:` field records the
**dispatch ref's** SHA (main at dispatch time), not the commit actually built
from the `branch` input — trust the `Version:` line and the input branch, not
that field.

## 3. Hardware verification — dev kit first, always

Download the pre-release assets and OTA-flash the **release** image to the
dev kit with the `flash` skill (never a production unit first):

```
gh release download vX.Y.Z-<run#> -p '*release.signed.bin' -D <dir>
```

Then run `tools/verify.py --name "<device>"` — it must print `fw X.Y.Z` and
`repl-ok`. For provenance, also probe `frame.GIT_TAG` over the REPL: it is
the app repo's **12-char commit hash** at build time (`APP_BUILD_VERSION`
from `git describe --abbrev=12 --always` — hash-only, because plain
`git describe` ignores lightweight tags and this repo's tags are
lightweight). It should match the built commit; an **empty string** means
the build's `git describe` silently failed (CI images of 0.8.8 and earlier
all have this — fixed by the `safe.directory` line in the workflow). The
MCUboot image hash `ota_flash.py` prints is the other provenance anchor.
Then roll out to production units as desired (their `main.lua` apps
survive OTA).

## 4. Tag

Lightweight tag named exactly `X.Y.Z` on the **bump commit's merge on main**:

```
git -C alif fetch origin && git -C alif tag X.Y.Z origin/main && git -C alif push origin X.Y.Z
```

(Confirm `origin/main` HEAD is the bump PR's merge commit first; if later PRs
have landed, tag the merge commit of the bump PR instead.)

## 5. Final GitHub release

Rename the two OTA payloads to the short convention — these are the only two
assets (bootloader bins stay on the pre-release):

- `halo-firmware-X.Y.Z-release.signed.bin` → `X.Y.Z.bin`
- `halo-firmware-X.Y.Z-debug.signed.bin` → `X.Y.Z-debug.bin`

Body = hand-written summary + generated PR list. Write the body to a file
(never inline heredocs with backticks):

```
gh release create X.Y.Z <dir>/X.Y.Z.bin <dir>/X.Y.Z-debug.bin \
  --title "Release X.Y.Z" --notes-file <file> --generate-notes --latest
```

Body format (see 0.8.7/0.8.8 for reference): `## Halo firmware X.Y.Z`,
"Compared to \`prev\` (\`<sha>\`)." line, `### Highlights`, `### Lua / app
notes` (API changes apps must react to), optional `### Ops`, link to
CHANGELOG.md. `--generate-notes` appends the "What's Changed" PR list and the
Full Changelog compare link below your notes.

## Gotchas

- **The final release is created manually** — the workflow only makes
  pre-releases. Don't skip the asset rename.
- The pre-release can be built from the bump **branch** before the PR merges
  (input `branch=<bump-branch>`) when parallelizing; content is identical as
  long as the PR merges as a merge commit with no other PRs landing in between.
- The workflow runs on `workflow_dispatch` only — nothing releases
  automatically on tag push.
- Kconfig-default changes are safe here: CI always builds pristine.
- `VERSION_TWEAK` non-zero appends `.TWEAK`; `EXTRAVERSION` appends `-<str>` —
  both should be 0/empty for a normal release.
- Mind MCUboot test-boot semantics when verifying: an image that crashes
  before self-confirm is rolled back on the next boot (see the flash skill).
