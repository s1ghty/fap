# fap — C Package Manager

## What this is
A minimal, opinionated package manager written in C. Hybrid declarative/imperative model.
No daemon, no runtime, no bloat.

## Design decisions (settled, do not revisit)
- Target: **Linux x86_64 only**. No cross-platform support, no arch/OS detection anywhere in the codebase — don't add any.
- Written in **C99**
- **fap.toml** — declarative manifest (what you want), lives at `<root>/fap.toml` (see Install root below) — machine-wide, not per-directory (fap is a system package manager, not a per-project dependency tool; running `fap install` from two different directories, at the same privilege level, must see the same state, not create two). `fap install`/`fap remove` also keep it in sync (add/drop a bare, unpinned entry) so it always reflects your explicit installs, NixOS-`configuration.nix`-style — copy just this file to another machine and `fap sync` reproduces the same top-level packages. Only top-level requested packages are recorded, never the transitive deps a resolve pulled in alongside them — those are re-derived from the registry every time, not tracked declaratively.
- **fap.lock** — lockfile (exact resolved versions + hashes), lives at `<root>/fap.lock`, same machine-wide reasoning as fap.toml
- Two channels: **stable** and **edge**
- Atomic installs via **staging directory** — fully written before any symlink swap
- Compression: **zstd**
- Integrity: **SHA256** verification on every package
- **Install root: dual-mode, chosen at runtime by effective privilege** (`geteuid() == 0`), not a flag or env var — the same signal every other package manager already uses (`sudo <cmd>` vs plain `<cmd>`), and it naturally does the right thing inside an LFS chroot too, where you're root the entire time.
  - **Root/sudo** → `/var/lib/fap/` (packages in `/var/lib/fap/pkgs/<name>-<version>/`), binaries symlinked into `/usr/local/bin/`. Deliberately **not** `/usr` — that stays untouched so fap never contends with the host distro's own package manager (portage, apt, pacman, ...) over file ownership when run as a secondary package manager.
  - **Everyone else** → `~/.local/fap/` (unchanged from fap's original design), binaries in `~/.local/bin/`.
  - Both resolved through `fap_root_path()`/`fap_bin_path()` (see `util.c`) — nothing else should ever hardcode `~/.local` or `/var/lib/fap` directly.
- Binaries symlinked into the active bin dir — except a package with bundled libs (see below), which gets wrapper scripts instead. Symlink/wrapper targets are always **absolute**, never relative — in system mode the bin dir and the package root aren't siblings the way `~/.local/bin` and `~/.local/fap` are.
- Bundled shared libraries (`pkg.libs`) are copied into one shared `<root>/libs/`; found at runtime via `LD_LIBRARY_PATH` in a generated wrapper script. In user mode this is because `/etc/ld.so.conf.d` needs root; in system mode fap *has* root, so switching this specific mechanism to a real `ld.so.conf.d` entry + `ldconfig` is a reasonable future improvement — not done yet, wrapper scripts still work correctly in system mode too, just not the most idiomatic choice there.
- No superuser required for user-local installs — unchanged. System-wide installs use root/sudo, same as every other system package manager; this is additive to the design, not a reversal of it.

## CLI surface (settled)
```
fap install [pkg]       # install package(s), update lock and fap.toml
fap remove [pkg]        # remove package, update lock and fap.toml
fap sync                # install everything in fap.toml (from lock if exists)
fap update [pkg]        # upgrade to latest in channel, update lock
fap search <query>      # search registry
fap list                # list installed packages
fap info <pkg>          # show package metadata
fap channels            # list available channels and their index URLs
```

## Repository layout
```
fap/
├── CLAUDE.md           ← you are here
├── Makefile
├── fap.toml            ← example manifest
├── include/
│   ├── fap.h           ← shared types and constants
│   └── json.h          ← JSON parsing API (json.c)
├── src/
│   ├── main.c          ← CLI entrypoint, arg dispatch
│   ├── cli.c           ← command implementations
│   ├── config.c        ← fap.toml parsing (toml.h via vendor)
│   ├── lock.c          ← fap.lock read/write
│   ├── json.c          ← minimal JSON parser (channel index, fap.lock)
│   ├── registry.c      ← fetch + parse channel index, local index cache
│   ├── resolver.c      ← dependency resolution (pkg.deps → install order)
│   ├── package.c       ← download, verify SHA256, extract zstd tarball
│   ├── install.c       ← staging → atomic swap → symlink → .desktop entry
│   ├── hash.c          ← SHA256 implementation (or libcrypto wrapper)
│   └── util.c          ← path helpers, string utils, error handling
├── vendor/
│   ├── toml.h          ← TOML parser API (toml-c)
│   └── toml.c          ← TOML parser implementation
└── tests/
    ├── test_hash.c
    ├── test_lock.c
    ├── test_config.c
    ├── test_registry.c
    ├── test_resolver.c
    ├── test_package.c
    └── test_install.c  ← 180 checks total across all 7 files
```

## fap.toml format
Lives at the active install root's `fap.toml` at runtime — `/var/lib/fap/fap.toml` under root, `~/.local/fap/fap.toml` otherwise (the copy in this repo's root is just a format example, not something fap ever reads from a project directory).
```toml
[package]
name = "myproject"

[dependencies]
curl = { version = "8.6.0", channel = "stable" }
jq   = { channel = "stable" }   # latest on stable
eza  = { channel = "edge" }   # latest on edge
```

## fap.lock format (JSON)
```json
{
  "lockfile_version": 1,
  "packages": [
    {
      "name": "curl",
      "version": "8.6.0",
      "channel": "stable",
      "url": "https://...",
      "sha256": "abc123...",
      "bin": ["curl"],
      "libs": [],
      "deps": ["openssl", "zlib"]
    }
  ]
}
```
`bin`, `libs`, and `deps` are optional arrays (older lockfiles without them still load fine).

## Channel index format (served by registry, JSON)
```json
{
  "channel": "stable",
  "packages": [
    {
      "name": "curl",
      "version": "8.6.0",
      "url": "https://...",
      "sha256": "abc123...",
      "description": "...",
      "bin": ["curl"],
      "libs": ["libcurl.so.4"],
      "deps": ["openssl", "zlib"],
      "desktop_type": "application",
      "desktop_name": "Curl",
      "icon": "browser/chrome/icons/default/default128.png"
    }
  ]
}
```
`deps` lists the names of other packages this one depends on (resolved
against the same registry, no version constraints — same model as
`fap.toml`'s bare `channel`-only deps). `libs` lists bundled shared
library filenames, found at `lib/<name>` inside the package (mirrors
how `bin` entries are found at `bin/<name>`) — see the install root
bullet above for what happens to them at install time. `desktop_type`
(one of `application`, `x11-session`, `wayland-session`) and
`desktop_name` register an XDG `.desktop` entry at install time —
launcher/menu visibility for `application` (both privilege modes), or
login-manager session selection for the two session types (system
mode only — see `install.c`'s `install_desktop_entry()`). `icon` is a
path to an icon file already inside the package (relative to the
package root, same bare-name-or-explicit-path convention as `bin`/
`libs`) — if set, becomes an absolute `Icon=` line in the `.desktop`
entry pointing straight at the installed file, no icon theme involved.
`deps`, `bin`, `libs`, `desktop_type`, `desktop_name`, and `icon` are
all optional; most packages set none of the last three.

## Companion repo: fap-registry
This repo (`fap`) is the client only — it never authors packages. The
actual `stable.json` served at `FAP_DEFAULT_STABLE_INDEX_URL` (see
registry.c) is built and hosted by a **separate sibling repo**,
`s1ghty/fap-registry`, checked out independently (not a subdirectory
or submodule of this repo — different local path entirely, e.g.
`~/Downloads/project/fap-registry` in this environment, but always
just wherever it happens to be cloned; look for it rather than assume
a path).

- `package.sh` — turns one already-downloaded/built binary (or a whole
  `--tree` app directory) into a fap package tarball + a `stable.json`
  entry, handling `ldd`-based shared-lib bundling (with a GPU/graphics-
  driver-stack exclusion list — those libs must always come from the
  target machine, never get bundled), and `--desktop-type`/
  `--desktop-name`/`--icon` for `.desktop` entries.
- `update.sh` — reads `sources.toml`, checks each package's upstream
  (GitHub releases API, or an arbitrary `version_url` for non-GitHub
  projects like Firefox) for a newer version, and if found: downloads/
  builds it, repackages via `package.sh`, uploads the tarball as a
  GitHub release asset, updates `stable.json`, and opens a PR. Runs on
  a schedule via GitHub Actions; `-f, --force NAME` re-runs it for one
  package immediately regardless of whether upstream actually has a
  newer version — for when *packaging logic itself* changed (a new
  `package.sh` flag, a bundling-exclusion fix), not the upstream
  release. A forced repackage with the version unchanged gets its own
  disambiguated release tag rather than overwriting the existing one
  in place — a previously-published URL's content must never change
  after the fact, since that's exactly what fap.lock's sha256 field
  (and any index cache) assumes holds everywhere.
- `sources.toml` — one `[[package]]` table per tracked upstream, field
  docs live at the top of the file itself.

**Before merging any fap-registry PR that changes `stable.json`**
(automated update PRs included), validate it against fap's *real*
parser, not just by eyeballing the JSON — a single malformed entry
breaks index parsing for every fap user, not just that one package.
Pattern used throughout this project's history: build a small
throwaway C program that links this repo's actual `registry.c`/
`json.c`/`util.c`, point `FAP_STABLE_INDEX_URL` at the PR branch's raw
`stable.json` on GitHub, call `fap_index_fetch`, and confirm it parses
cleanly with the expected field values for the changed package(s).
`FapIndex` is large (heap-allocate it — `malloc(sizeof(FapIndex))`,
never declare one on the stack, see `cli.c` for the pattern every real
caller already uses). Also watch for index-cache collisions across
repeated ad-hoc validation runs: every call shares one cache path
independent of the URL fetched (real fap usage only ever points at one
registry, so this is by design) — `rm -rf ~/.local/fap/index-cache`
(or whatever `$HOME` the harness runs under) between runs when
pointing at different branches/URLs in sequence, or a stale result
from an earlier check will silently get served for a later one.

## Coding conventions
- C99, no C++ features, no GCC extensions unless explicitly noted
- All error paths return -1 and set a global `fap_err` string (see include/fap.h)
- No dynamic allocation without a corresponding free path
- Prefer `snprintf` over `sprintf`, always check return values
- Functions that can fail return `int` (0 = ok, -1 = err); output via pointer param
- One .c per logical subsystem, matching .h in include/
- No global mutable state except `fap_err`

## Build
```sh
make          # builds ./fap binary
make test     # builds and runs tests
make clean
```

Dependencies: libcurl, libzstd, libcrypto (OpenSSL) — all standard on Linux.
TOML parsing: vendor/toml.h + toml.c (toml-c, vendored, no extra system dep).

## What's implemented vs TODO
See README.md for current status. When implementing, always write the .h declaration
before the .c body. Keep functions small — if a function exceeds ~60 lines, split it.

## Session workflow for Claude Code
1. Read this file first (you're doing that)
2. Check README.md for current implementation status
3. Run `make` to see current build state
4. Implement the task, keep diffs small and testable
5. Run `make test` after any change to hash.c or util.c
