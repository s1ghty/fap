#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "fap.h"
#include "toml.h"

/*
 * config.c — parse fap.toml into FapManifest, via vendor/toml.h (tomlc99)
 */

/* Copy a TOML string datum into a fixed buffer and free the datum.
 * Returns -1 (and sets fap_err) if it doesn't fit. */
static int copy_datum(toml_datum_t d, const char *field, char *out, size_t outsz)
{
    int n = snprintf(out, outsz, "%s", d.u.s);
    free(d.u.s);
    if (n < 0 || (size_t)n >= outsz)
        return fap_error("config: %s too long (max %zu)", field, outsz - 1);
    return 0;
}

static int load_dep(toml_table_t *deps, const char *key, FapDep *dep)
{
    memset(dep, 0, sizeof(*dep));
    if (snprintf(dep->name, sizeof(dep->name), "%s", key) >= (int)sizeof(dep->name))
        return fap_error("config: dependency name too long (max %d)", FAP_MAX_NAME - 1);

    toml_table_t *tab = toml_table_in(deps, key);
    if (!tab)
        return fap_error("config: dependency \"%s\" must be a table, e.g. { version = \"1.0\" }", key);

    toml_datum_t version = toml_string_in(tab, "version");
    if (version.ok && copy_datum(version, "dependency version", dep->version, sizeof(dep->version)) < 0)
        return -1;

    toml_datum_t channel = toml_string_in(tab, "channel");
    if (channel.ok) {
        char chan[16];
        int r = copy_datum(channel, "dependency channel", chan, sizeof(chan));
        if (r == 0)
            r = fap_channel_parse(chan, &dep->channel);
        if (r < 0)
            return -1;
    } else {
        dep->channel = FAP_CHANNEL_STABLE;
    }

    return 0;
}

int fap_manifest_load(const char *path, FapManifest *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return fap_error("config: open %s: %s", path, strerror(errno));

    char errbuf[256];
    toml_table_t *conf = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);
    if (!conf)
        return fap_error("config: %s: %s", path, errbuf);

    memset(out, 0, sizeof(*out));

    toml_table_t *pkg = toml_table_in(conf, "package");
    if (!pkg) {
        toml_free(conf);
        return fap_error("config: %s missing [package] section", path);
    }
    toml_datum_t name = toml_string_in(pkg, "name");
    if (!name.ok) {
        toml_free(conf);
        return fap_error("config: %s: [package] missing \"name\"", path);
    }
    if (copy_datum(name, "package name", out->project_name, sizeof(out->project_name)) < 0) {
        toml_free(conf);
        return -1;
    }

    toml_table_t *deps = toml_table_in(conf, "dependencies");
    if (deps) {
        for (int i = 0; ; i++) {
            const char *key = toml_key_in(deps, i);
            if (!key)
                break;
            if (out->deps_count >= FAP_MAX_DEPS) {
                toml_free(conf);
                return fap_error("config: too many dependencies (max %d)", FAP_MAX_DEPS);
            }
            if (load_dep(deps, key, &out->deps[out->deps_count]) < 0) {
                toml_free(conf);
                return -1;
            }
            out->deps_count++;
        }
    }

    toml_free(conf);
    return 0;
}
