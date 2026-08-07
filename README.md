# fap

Minimal C package manager. No daemon, no runtime, no bullshit.

## Status

| Module       | File             | Status        |
|-------------|------------------|---------------|
| Types/API    | include/fap.h    | ✅ done        |
| Util/paths   | src/util.c       | ✅ done        |
| SHA256       | src/hash.c       | ✅ done        |
| CLI dispatch | src/main.c       | ✅ done        |
| CLI commands | src/cli.c        | ✅ done        |
| TOML parser  | src/config.c     | ✅ done        |
| Lock file    | src/lock.c       | ✅ done        |
| JSON helpers | src/json.c       | ✅ done        |
| Registry     | src/registry.c   | ✅ done        |
| Dep resolver | src/resolver.c   | ✅ done        |
| Download     | src/package.c    | ✅ done        |
| Install      | src/install.c    | ✅ done        |
| Tests        | tests/*.c        | ✅ done (125 checks across 7 files) |

## Build

```sh
# Dependencies (Arch/NixOS)
# pacman -S openssl curl zstd
# nix-shell -p openssl curl zstd

make
make test
```

## Install dir layout

```
~/.local/
├── bin/
│   ├── curl -> ../fap/pkgs/curl-8.6.0/bin/curl     (plain symlink, no libs)
│   └── mytool                                       (wrapper script, has libs — see below)
└── fap/
    ├── staging/          (temp during install, always cleaned up)
    ├── libs/             (bundled .so files, shared across all packages)
    │   └── libmytool.so.1
    └── pkgs/
        ├── curl-8.6.0/
        │   └── bin/
        │       └── curl
        └── mytool-1.0.0/
            ├── bin/
            │   └── mytool
            └── lib/
                └── libmytool.so.1
```

A package with no `libs` gets its binaries symlinked into `~/.local/bin/`
directly, same as always. A package that declares `libs` gets wrapper
scripts instead — `~/.local/bin/mytool` is then a small shell script that
sets `LD_LIBRARY_PATH` to include `~/.local/fap/libs/` before exec'ing the
real binary in `pkgs/mytool-1.0.0/bin/`. This is because there's no
per-user way to extend the dynamic linker's search path — `/etc/ld.so.conf.d`
is system-wide and needs root, which fap's design rules out.

## Implementation order (recommended for Claude Code sessions)

1. `src/lock.c`     — pure JSON read/write, no network, testable in isolation
2. `src/config.c`   — TOML parse with vendor/toml.h, needs vendor dir setup first
3. `src/registry.c` — libcurl fetch + JSON parse of channel index
4. `src/package.c`  — libcurl download + zstd extract + SHA256 verify
5. `src/install.c`  — staging dir logic + rename + symlinks
6. `src/cli.c`      — wire up commands to the above
7. More tests

## Registry config

Channel index URLs aren't hardcoded — set `FAP_STABLE_INDEX_URL` /
`FAP_EDGE_INDEX_URL` before running `fap search`, `install`, `sync`,
`update`, or `info`. `fap channels` shows what's currently configured.
Only one channel needs to be set; commands that check both (like
`fap search`) just skip whichever one isn't configured.

## Timing

Set `FAP_TIME=1` to print how long a command took, e.g.
`FAP_TIME=1 fap install jq` → `install: 842ms`. Off by default, no
extra output otherwise.
