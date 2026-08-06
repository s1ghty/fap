# fap — C Package Manager

## What this is
A minimal, opinionated package manager written in C. Hybrid declarative/imperative model.
No daemon, no runtime, no bloat.

## Design decisions (settled, do not revisit)
- Target: **Linux x86_64 only**. No cross-platform support, no arch/OS detection anywhere in the codebase — don't add any.
- Written in **C99**
- **fap.toml** — declarative manifest (what you want)
- **fap.lock** — lockfile (exact resolved versions + hashes)
- Two channels: **stable** and **edge**
- Atomic installs via **staging directory** — fully written before any symlink swap
- Compression: **zstd**
- Integrity: **SHA256** verification on every package
- Install root: `~/.local/fap/` (packages go in `~/.local/fap/pkgs/<name>-<version>/`)
- Binaries symlinked into `~/.local/bin/` — except a package with bundled libs (see below), which gets wrapper scripts instead
- Bundled shared libraries (`pkg.libs`) are copied into one shared `~/.local/fap/libs/`; found at runtime via `LD_LIBRARY_PATH` in a generated wrapper script, never via `/etc/ld.so.conf.d` — that's system-wide and needs root
- No superuser required

## CLI surface (settled)
```
fap install [pkg]       # install package(s), update lock
fap remove [pkg]        # remove package, update lock
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
```toml
[package]
name = "myproject"

[dependencies]
curl = { version = "8.6.0", channel = "stable" }
jq   = { version = "1.7",   channel = "stable" }
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
      "deps": ["openssl", "zlib"]
    }
  ]
}
```
`deps` lists the names of other packages this one depends on (resolved
against the same registry, no version constraints — same model as
`fap.toml`'s bare `channel`-only deps). `libs` lists bundled shared
library filenames, found at `lib/<name>` inside the package (mirrors
how `bin` entries are found at `bin/<name>`) — see the install root
bullet above for what happens to them at install time. `deps`, `bin`,
and `libs` are all optional.

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
