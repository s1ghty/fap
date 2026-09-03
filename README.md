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
| Tests        | tests/*.c        | ✅ done (206 checks across 8 files) |

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

## Autoremove

`fap remove <pkg>` only ever removes exactly the package you named —
if it pulled in dependencies nobody else needs anymore, those stay
installed, same as `apt remove` (not `apt autoremove`) or plain
`pacman -R`. Run `fap autoremove` separately to clean those up: it
removes every installed package that both (a) isn't itself listed in
`fap.toml`, and (b) isn't a dependency, even transitively, of anything
that is. `fap remove` prints a note when it leaves orphans behind, the
same way it already nudges you toward `hash -r`.

```
$ fap remove firefox
removing firefox...
note: if a just-removed command still resolves, run: hash -r
note: 2 packages no longer needed by anything installed — run `fap autoremove` to remove them

$ fap autoremove
removing some-firefox-only-dependency (no longer needed)...
removing another-one (no longer needed)...
```

A whole chain (A only needed B, B only needed C) comes back and gets
removed together in one `fap autoremove` — no need to run it more than
once to fully unwind a chain.

## Registry config

`stable` works out of the box, no setup needed — it defaults to fap's
own registry (`FAP_DEFAULT_STABLE_INDEX_URL` in `fap.h`). Set
`FAP_STABLE_INDEX_URL` to point at a different one instead (a private
or self-hosted registry, for example). `edge` has no default yet — no
`edge.json` exists in the registry — so `FAP_EDGE_INDEX_URL` is
required until one does. `fap channels` shows what's actually in
effect for both. Commands that check both channels (like `fap search`)
just skip whichever one isn't configured, rather than erroring.

Every fetched index is cached locally for `FAP_INDEX_TTL` seconds
(default 3600 = 1 hour) — a command run again inside that window reads
the cache instead of hitting the network at all, no user-visible
difference besides speed. Set `FAP_INDEX_TTL=0` to always fetch fresh.
A fetch that fails outright (network down, registry unreachable) falls
back to the last cached copy if one exists, rather than failing the
command entirely.

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

## Desktop entries (app launchers, WM/compositor login sessions)

Most packages just install a binary — nothing more. A package can
optionally register an XDG `.desktop` entry too, via two fields in its
registry entry: `desktop_type` (`"application"`, `"x11-session"`, or
`"wayland-session"`) and `desktop_name` (the display name shown in a
launcher/session picker; falls back to the package name if unset).

- `"application"` — visible to app launchers (fuzzel, rofi, wofi, ...)
  and desktop menus. Works in both privilege modes: system installs
  write to `/usr/share/applications/`, per-user installs write to
  `~/.local/share/applications/`, the real XDG-defined per-user
  override for this one.
- `"x11-session"` / `"wayland-session"` — a WM/compositor becomes
  selectable at a display manager's (SDDM, GDM, LightDM, ...) login
  screen, writing to `/usr/share/xsessions/` or
  `/usr/share/wayland-sessions/`. **System mode (`sudo`) only** — a
  login manager reads these before any user is authenticated, so
  there's no meaningful per-user equivalent to fall back to; `fap
  install` fails cleanly rather than silently skipping if attempted
  without root.

`Exec=`/`TryExec=` always point at the package's actual installed
binary (its first declared `bin` entry, resolved the same way the
`bin_root` symlink is), so the entry is never stale relative to what
was really installed. The file is named `<package-name>.desktop`,
deterministically, so `fap remove` can find and delete it on removal
without needing to record which type (if any) was ever registered.

A third, independent field, `icon`, points at an icon file already
sitting inside the package (a path relative to the package root, e.g.
`"firefox/browser/chrome/icons/default/default128.png"`) and, if set,
writes an absolute `Icon=` line pointing straight at it — no icon
theme, no separate install step, same "absolute path is valid Icon=
syntax" trick real apps installed outside a theme dir (Chrome, VS
Code) already use. Optional; only matters alongside `desktop_type =
"application"` for launcher icon visibility, and only for a package
that actually ships one.
