# openSUSE / OBS packaging

The OBS package is built **directly from this git repo** via the
obs-scm-bridge (`scmsync`). Nothing is uploaded to OBS by hand and no
source tarball is stored anywhere — the bridge clones the repo, and the
build reconstructs the source tarball in the build VM.

The build recipe therefore lives at the **repo root**, where the bridge
can find it:

- `/nordstjernen.spec` — RPM build recipe (meson build).
- `/_service` — buildtime `tar` + `recompress` + `set_version` only. No
  network fetch service: the source comes from the scmsync checkout, and
  these services repackage it into `nordstjernen-<version>.tar.gz` inside
  the build VM.

## One-time OBS setup

The package must be named lowercase **`nordstjernen`** (so the
bridge-generated tarball is `nordstjernen-<version>` and matches the
spec's `%prep`). Create it if needed, then bind it to git:

    osc meta pkg home:andreasrosdal nordstjernen -e

and add inside `<package>`:

    <scmsync>https://github.com/nordstjernen-web/nordstjernen?trackingbranch=main</scmsync>

That is the whole setup. After it, git is authoritative: edit the spec or
`_service` in this repo and push — never touch files in the OBS web UI.
OBS follows `main` and rebuilds when it advances; for instant rebuilds add
a git webhook backed by `osc token --create --operation runservice`.

## Why not a `_service` that fetches from git

build.opensuse.org does not run network-fetching source services
(`tar_scm` / `obs_scm`) on its source server for this package — they
produce no archive, so the chain dies at `recompress`
("no such file … nordstjernen-*.tar") or at the buildtime `tar`
("no .obsinfo file found"). The scmsync bridge is a separate, working
code path for cloning git, so the source comes from there and the
`_service` only repackages it.

## License caveat

Nordstjernen is under the **Nordstjernen Source License v1.0 (NSL-1.0)**,
a source-available license that forbids "Competing Use" and restricts some
purposes to non-commercial use. It is **not** OSI-approved / free software,
so the spec tags it `SUSE-NonFree`. It can live in a home: project but will
**not** be accepted into openSUSE:Factory / Tumbleweed, whose legal review
only admits free licenses. NSL-1.0 converts to MIT ten years after each
release.

## Build options

The spec disables one meson feature unavailable in OBS:

- `-Dwebgpu=disabled` — needs external wgpu-native.

Everything else (GTK shell, sandboxed renderer, SDL2 audio helper, WebGL,
enchant spell-check) builds from the declared `BuildRequires`.
