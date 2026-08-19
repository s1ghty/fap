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
| Tests        | tests/*.c        | ✅ done (151 checks across 7 files) |

## Build

```sh
# Dependencies (Arch/NixOS)
# pacman -S openssl curl zstd
# nix-shell -p openssl curl zstd

make
make test
```

## System-wide vs. per-user

fap picks its install root at runtime from effective privilege — no flag,
no env var, just `geteuid() == 0`, the same signal every `sudo <cmd>` vs
plain `<cmd>` already relies on:

| | Root / `sudo` | Everyone else |
|---|---|---|
| Packages | `/var/lib/fap/pkgs/` | `~/.local/fap/pkgs/` |
| State (`fap.toml`, `fap.lock`, `libs/`) | `/var/lib/fap/` | `~/.local/fap/` |
| Binaries | `/usr/local/bin/` | `~/.local/bin/` |

System mode deliberately uses `/usr/local`, never `/usr` — that stays
untouched so fap never contends with the host distro's own package
manager over file ownership when run alongside it. Everything below
uses the per-user paths as the example; system mode is the same shape
rooted at `/var/lib/fap` + `/usr/local/bin` instead.

## Install dir layout

```
~/.local/
├── bin/
│   ├── curl -> /home/user/.local/fap/pkgs/curl-8.6.0/bin/curl   (plain symlink, no libs)
│   └── mytool                                                    (wrapper script, has libs — see below)
└── fap/
    ├── fap.toml          (declarative manifest, kept in sync by install/remove)
    ├── fap.lock          (exact resolved versions + hashes)
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

Symlink/wrapper targets are always **absolute** (never a relative
`../fap/pkgs/...` path) — in system mode the bin dir and the package
root aren't siblings the way `~/.local/bin` and `~/.local/fap` are, so
a relative target would resolve to the wrong place.

A package with no `libs` gets its binaries symlinked into the active bin
directory directly, same as always. A package that declares `libs` gets
wrapper scripts instead — `mytool` is then a small shell script that sets
`LD_LIBRARY_PATH` to include the active `libs/` dir before exec'ing the
real binary in `pkgs/mytool-1.0.0/bin/`. In user mode this is because
there's no per-user way to extend the dynamic linker's search path —
`/etc/ld.so.conf.d` is system-wide and needs root. In system mode fap
*has* root, so a real `ld.so.conf.d` entry would be more idiomatic there —
not done yet; wrapper scripts still work correctly in system mode too.

## Implementation order (recommended for Claude Code sessions)

1. `src/lock.c`     — pure JSON read/write, no network, testable in isolation
2. `src/config.c`   — TOML parse with vendor/toml.h, needs vendor dir setup first
3. `src/registry.c` — libcurl fetch + JSON parse of channel index
4. `src/package.c`  — libcurl download + zstd extract + SHA256 verify
5. `src/install.c`  — staging dir logic + rename + symlinks
6. `src/cli.c`      — wire up commands to the above
7. More tests

## Moving to a new machine

`fap.toml` and `fap.lock` both live at the active install root (see
above) — machine-wide, not per-directory. `fap install <pkg>` and
`fap remove <pkg>` keep `fap.toml` in sync with what you've explicitly
asked for (creating it with a default `[package]` section the first
time, if it didn't exist), so running `fap install` from any directory,
at the same privilege level, always sees the same state instead of
scattering a separate manifest/lock into whatever folder you happened
to be standing in. To reproduce the same set of top-level packages
elsewhere: copy `fap.toml` to the new machine and run `fap sync` at
the same privilege level you copied it from (system-wide `fap.toml`
→ `sudo fap sync`, per-user → plain `fap sync`). Transitive dependencies
aren't written into it — those get re-resolved from the registry every
time, same as `pkg.deps` always has.

## Registry config

`stable` works out of the box, no setup needed — it defaults to fap's
own registry (`FAP_DEFAULT_STABLE_INDEX_URL` in `fap.h`). Set
`FAP_STABLE_INDEX_URL` to point at a different one instead (a private
or self-hosted registry, for example). `edge` has no default yet — no
`edge.json` exists in the registry — so `FAP_EDGE_INDEX_URL` is
required until one does. `fap channels` shows what's actually in
effect for both. Commands that check both channels (like `fap search`)
just skip whichever one isn't configured, rather than erroring.

## Timing

Set `FAP_TIME=1` to print how long a command took, e.g.
`FAP_TIME=1 fap install jq` → `install: 842ms`. Off by default, no
extra output otherwise.

Under `sudo`, set it *after* `sudo` rather than before, or pass `-E`:
plain `sudo` resets the target user's environment by default (same
reason `FAP_STABLE_INDEX_URL` needs the same treatment — see Registry
config above), so `FAP_TIME=1 sudo fap install jq` silently drops it.
Either `sudo FAP_TIME=1 fap install jq` or
`FAP_TIME=1 sudo -E fap install jq` gets it through.

## Download progress

Installing a package that needs downloading shows live progress
(percentage and MB transferred) on a single, self-updating line, e.g.
`downloading firefox 154.0...  62.3%  (205.4/330.0 MB)`. Only when
stderr is an actual terminal — piped or redirected output (scripts,
logs, `fap install foo 2>&1 | tee log`) stays clean, same convention
`curl` itself uses.
