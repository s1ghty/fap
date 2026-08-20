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
│   └── fap.h           ← shared types and constants
├── src/
│   ├── main.c          ← CLI entrypoint, arg dispatch
│   ├── cli.c           ← command implementations
│   ├── config.c        ← fap.toml parsing (toml.h via vendor)
│   ├── lock.c          ← fap.lock read/write
│   ├── registry.c      ← fetch + parse channel index
│   ├── package.c       ← download, verify SHA256, extract zstd tarball
│   ├── install.c       ← staging → atomic swap → symlink
│   ├── hash.c          ← SHA256 implementation (or libcrypto wrapper)
│   └── util.c          ← path helpers, string utils, error handling
├── vendor/
│   └── toml.h          ← single-header TOML parser (toml-c)
└── tests/
    └── test_hash.c     ← unit tests (more to be added)
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
TOML parsing: vendor/toml.h (single-header, no extra dep).

## What's implemented vs TODO
See README.md for current status. When implementing, always write the .h declaration
before the .c body. Keep functions small — if a function exceeds ~60 lines, split it.

## Session workflow for Claude Code
1. Read this file first (you're doing that)
2. Check README.md for current implementation status
3. Run `make` to see current build state
4. Implement the task, keep diffs small and testable
5. Run `make test` after any change to hash.c or util.c
