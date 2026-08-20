#ifndef FAP_H
#define FAP_H

#include <stddef.h>
#include <stdint.h>

/* ── version ──────────────────────────────────────────────────── */
#define FAP_VERSION "0.1.0"

/* ── paths ────────────────────────────────────────────────────── */
/* Everything fap manages lives under one root, chosen at runtime by
 * effective privilege (see fap_root_path()/fap_bin_path() in util.c):
 *   - root/sudo   -> FAP_SYSTEM_ROOT + FAP_SYSTEM_BIN, machine-wide.
 *     Deliberately not /usr — that stays untouched so fap never
 *     contends with the host distro's own package manager over file
 *     ownership when run as a secondary package manager.
 *   - anyone else -> FAP_USER_ROOT + FAP_USER_BIN under $HOME,
 *     unchanged from fap's original per-user design.
 * The constants below are the sub-paths under whichever root is
 * active; nothing outside util.c should reference FAP_USER_ROOT/
 * FAP_SYSTEM_ROOT/FAP_USER_BIN/FAP_SYSTEM_BIN directly — always go
 * through fap_root_path()/fap_bin_path() so both modes stay in sync. */
#define FAP_USER_ROOT   ".local/fap"
#define FAP_USER_BIN    ".local/bin"
#define FAP_SYSTEM_ROOT "/var/lib/fap"
#define FAP_SYSTEM_BIN  "/usr/local/bin"

#define FAP_PKGS        "pkgs"
#define FAP_STAGING     "staging"
#define FAP_LIBS        "libs"

/* XDG desktop-entry directories a package can optionally register
 * into (see pkg->desktop_type below and fap_desktop_dir() in util.c).
 * "application" (launcher/menu visibility, e.g. fuzzel/rofi finding
 * an installed GUI app) works in both privilege modes, same
 * system-vs-user split as everything else — XDG defines a real
 * per-user override dir for this one. "x11-session"/"wayland-session"
 * (a WM/compositor becoming selectable at a display manager's login
 * screen) only make sense in system mode: a login manager reads these
 * before any user is authenticated, so there's no meaningful
 * per-user equivalent to fall back to. */
#define FAP_SYSTEM_APPLICATIONS  "/usr/share/applications"
#define FAP_USER_APPLICATIONS   ".local/share/applications"
#define FAP_XSESSIONS           "/usr/share/xsessions"
#define FAP_WAYLAND_SESSIONS    "/usr/share/wayland-sessions"
/* fap tracks machine-wide state, like a distro package manager, not a
 * per-project dependency file — running `fap install` from two
 * different directories (or as two different users under the same
 * privilege level) must see the same manifest/lock, not create a
 * second copy. */
#define FAP_MANIFEST    "fap.toml"
#define FAP_LOCK        "fap.lock"

/* ── limits ───────────────────────────────────────────────────── */
#define FAP_MAX_NAME    128
#define FAP_MAX_VERSION 64
#define FAP_MAX_URL     2048
#define FAP_MAX_PATH    4096
#define FAP_SHA256_HEX  65   /* 64 hex chars + NUL */
#define FAP_MAX_BINS    32   /* max binaries per package */
/* max bundled shared libraries per package — a real desktop-stack
 * package (sway, a Wayland compositor, is the case that first hit
 * this) can genuinely need dozens: every non-glibc-core .so its
 * entry binary links against gets bundled (see package.sh), and a
 * compositor's dependency graph runs through cairo/pango/glib,
 * wlroots, X11-compat libs for Xwayland, and more — 69 in sway's
 * case. 128 leaves real headroom above that, not just enough to
 * scrape by. */
#define FAP_MAX_LIBS    128
#define FAP_MAX_DEPS    256  /* max deps in one manifest */
#define FAP_MAX_PKGS    1024 /* max packages in a channel index */
#define FAP_MAX_PKG_DEPS 32  /* max declared dependencies per package */

/* ── channels ─────────────────────────────────────────────────── */
typedef enum {
    FAP_CHANNEL_STABLE = 0,
    FAP_CHANNEL_EDGE   = 1,
} FapChannel;

/* ── package descriptor ───────────────────────────────────────── */
typedef struct {
    char       name[FAP_MAX_NAME];
    char       version[FAP_MAX_VERSION];
    FapChannel channel;
    char       url[FAP_MAX_URL];
    char       sha256[FAP_SHA256_HEX];
    char       description[512];
    /* Each entry is either a bare name (found at bin/<name> in the
     * package, the common single-binary-package case) or, if it
     * contains a '/', an explicit path relative to the package root
     * (e.g. "firefox/firefox") — for a package that ships a real
     * directory tree of resources alongside its binary rather than
     * just bin/<name>. Either way the installed command is always the
     * basename: "nvim" and "bin/nvim" both end up runnable as "nvim".
     * See install.c's resolve_pkg_entry(). */
    char       bins[FAP_MAX_BINS][FAP_MAX_NAME];
    int        bins_count;
    char       libs[FAP_MAX_LIBS][FAP_MAX_NAME]; /* bundled .so filenames, found at lib/<name> in the package */
    int        libs_count;
    char       deps[FAP_MAX_PKG_DEPS][FAP_MAX_NAME]; /* names of packages this one depends on */
    int        deps_count;
    /* Optional XDG desktop-entry registration — empty (the default)
     * for most packages, which just install a binary with no launcher/
     * session visibility at all. One of "application", "x11-session",
     * "wayland-session" (see the FAP_*_APPLICATIONS/FAP_*SESSIONS
     * comment above); anything else is treated as unset. desktop_name
     * is the human-readable Name= shown in the launcher/session
     * picker — falls back to pkg->name if left empty. */
    char       desktop_type[16];
    char       desktop_name[FAP_MAX_NAME];
    /* Optional path (bare name or, with a '/', explicit relative path —
     * same resolve_pkg_entry() rules as bins/libs above) to an icon
     * file inside the package, shown by launchers (fuzzel, rofi, ...)
     * next to a "application" desktop_type entry. Empty (the default)
     * means no Icon= line is written — most packages, and any
     * single-binary package with no bundled resource tree, have
     * nothing to point at. */
    char       icon[FAP_MAX_NAME];
} FapPackage;

/* ── dependency (from fap.toml) ───────────────────────────────── */
typedef struct {
    char       name[FAP_MAX_NAME];
    char       version[FAP_MAX_VERSION]; /* empty = latest */
    FapChannel channel;
} FapDep;

/* ── manifest (fap.toml) ──────────────────────────────────────── */
typedef struct {
    char   project_name[FAP_MAX_NAME];
    FapDep deps[FAP_MAX_DEPS];
    int    deps_count;
} FapManifest;

/* ── lockfile entry ───────────────────────────────────────────── */
typedef struct {
    FapPackage pkg;
} FapLockEntry;

typedef struct {
    FapLockEntry entries[FAP_MAX_DEPS];
    int          count;
} FapLock;

/* ── channel index ────────────────────────────────────────────── */
typedef struct {
    FapChannel  channel;
    FapPackage  pkgs[FAP_MAX_PKGS];
    int         count;
} FapIndex;

/* ── global error ─────────────────────────────────────────────── */
extern char fap_err[512];

/* Set fap_err and return -1 in one call */
int fap_error(const char *fmt, ...);

/* ── subsystem APIs (see individual headers) ──────────────────── */

/* config.h  — parse fap.toml into FapManifest */
int fap_manifest_load(const char *path, FapManifest *out);

/* Adds (or updates the channel of) a bare, unpinned dependency entry
 * and atomically rewrites path — creating it with a default
 * [package] section first if it doesn't exist yet. Used by `fap
 * install` so imperative installs are also recorded declaratively:
 * copy fap.toml to another machine, run `fap sync`, get the same
 * packages back. */
int fap_manifest_add_dep(const char *path, const char *name, FapChannel channel);

/* Removes a dependency entry if present and atomically rewrites path.
 * A missing file, or a name not found in it, is not an error — used
 * by `fap remove` as best-effort bookkeeping the same way lock_remove
 * is. */
int fap_manifest_remove_dep(const char *path, const char *name);

/* lock.h    — read/write fap.lock */
int fap_lock_load(const char *path, FapLock *out);
int fap_lock_save(const char *path, const FapLock *lock);

/* registry.h — fetch + parse channel index */

/* Baked-in default for the stable channel — fap's own registry, so
 * `fap install`/`search`/etc. work with zero configuration.
 * FAP_STABLE_INDEX_URL still overrides it, e.g. for a private/custom
 * registry. No default for edge: no edge.json exists in the registry
 * yet, so defaulting it would just point at a 404 — FAP_EDGE_INDEX_URL
 * is required until one does. */
#define FAP_DEFAULT_STABLE_INDEX_URL "https://raw.githubusercontent.com/s1ghty/fap-registry/main/stable.json"

/* Effective index URL for channel: the FAP_*_INDEX_URL env var if
 * set, else fap's own default for stable. NULL if the channel has
 * neither (always true for edge, until it gets a default too). */
const char *fap_channel_index_url(FapChannel channel);

int fap_index_fetch(FapChannel channel, FapIndex *out);
int fap_index_find(const FapIndex *idx, const char *name,
                   const char *version, FapPackage *out);

/* resolver.h — recursive dependency resolution + topological sort.
 * Deps are resolved against the same index they're declared in (no
 * per-dep channel/version — same model as fap.toml's bare deps).
 * On success, out->pkgs[0..out->count) is the install order:
 * dependencies always appear before whatever depends on them, and
 * shared dependencies appear exactly once. Fails on a missing
 * dependency or a circular one. */
int fap_resolve(const FapIndex *idx, const char *name, FapIndex *out);

/* package.h — download + verify */
int fap_package_download(const FapPackage *pkg, const char *dest_path);

/* install.h — staging → atomic swap → symlink
 *
 * A package's bundled .so files (pkg->libs) are copied into the
 * single shared ~/.local/fap/libs/ directory. ld.so.conf.d is
 * system-wide (under /etc) and needs root to write plus ldconfig to
 * apply — not an option for a no-superuser-required per-user install.
 * So a package with any libs gets its binaries installed as small
 * wrapper scripts that set LD_LIBRARY_PATH before exec'ing the real
 * binary, instead of plain symlinks. */
int fap_install(const FapPackage *pkg);
int fap_remove(const char *name);

/* hash.h    — SHA256 */
int fap_sha256_file(const char *path, char hex_out[FAP_SHA256_HEX]);
int fap_sha256_verify(const char *path, const char *expected_hex);

/* util.h    — paths, strings, fs helpers */
int  fap_home_path(const char *rel, char *buf, size_t bufsz);

/* 1 if fap is running with root privilege (system-wide mode), 0
 * otherwise (per-user mode). The single source of truth for which
 * install root is active — see the paths section above. */
int  fap_is_system_mode(void);

/* Resolves sub (e.g. "pkgs", "fap.lock") under the currently active
 * root — FAP_SYSTEM_ROOT if running as root, $HOME/FAP_USER_ROOT
 * otherwise. Pass NULL or "" for sub to get the root itself. */
int  fap_root_path(const char *sub, char *buf, size_t bufsz);

/* Same as fap_root_path(), but resolves under the *other* mode's root
 * than the one currently active. Diagnostic-only — e.g. so `fap
 * remove` can tell you a package exists but was installed at the
 * other privilege level, instead of just claiming it isn't installed
 * at all. Never used for actual reads or writes. */
int  fap_other_root_path(const char *sub, char *buf, size_t bufsz);

/* Resolves the bin directory binaries get symlinked/wrapper-scripted
 * into — FAP_SYSTEM_BIN if running as root, $HOME/FAP_USER_BIN
 * otherwise. */
int  fap_bin_path(char *buf, size_t bufsz);

/* Resolves the target directory for a .desktop entry of the given
 * type ("application", "x11-session", or "wayland-session" — see
 * FapPackage's desktop_type). "application" works in both privilege
 * modes; the two session types only make sense in system mode and
 * return -1 (fap_err set) otherwise — see the FAP_XSESSIONS/
 * FAP_WAYLAND_SESSIONS comment above for why. Also returns -1 for
 * any other/empty desktop_type. */
int  fap_desktop_dir(const char *desktop_type, char *buf, size_t bufsz);

int  fap_mkdir_p(const char *path);
int  fap_rm_rf(const char *path);
int  fap_copy_file(const char *src, const char *dst);
int  fap_symlink_force(const char *target, const char *link);
void fap_channel_str(FapChannel ch, char *buf, size_t bufsz);
int  fap_channel_parse(const char *s, FapChannel *out);

#endif /* FAP_H */
