#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "fap.h"

/* Forward declarations — implemented in cli.c */
int cmd_install(int argc, char **argv);
int cmd_remove(int argc, char **argv);
int cmd_sync(int argc, char **argv);
int cmd_update(int argc, char **argv);
int cmd_search(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_channels(int argc, char **argv);

static void usage(void)
{
    fprintf(stderr,
        "fap " FAP_VERSION " — C package manager\n"
        "\n"
        "Usage: fap <command> [args]\n"
        "\n"
        "Commands:\n"
        "  install [pkg...]   Install package(s), update lock\n"
        "  remove  [pkg...]   Remove package(s), update lock\n"
        "  sync               Install everything in fap.toml\n"
        "  update  [pkg...]   Upgrade package(s) to latest in channel\n"
        "  search  <query>    Search registry\n"
        "  list               List installed packages\n"
        "  info    <pkg>      Show package metadata\n"
        "  channels           List available channels\n"
        "\n"
    );
}

typedef struct {
    const char *name;
    int (*fn)(int, char **);
    /* Only commands that write fap.toml/fap.lock need to serialize
     * against a concurrent fap invocation (see fap_acquire_lock() in
     * util.c) — search/list/info/channels are read-only and stay
     * unblocked. */
    int needs_lock;
} Command;

static const Command commands[] = {
    { "install",  cmd_install,  1 },
    { "remove",   cmd_remove,   1 },
    { "sync",     cmd_sync,     1 },
    { "update",   cmd_update,   1 },
    { "search",   cmd_search,   0 },
    { "list",     cmd_list,     0 },
    { "info",     cmd_info,     0 },
    { "channels", cmd_channels, 0 },
    { NULL, NULL, 0 }
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("fap " FAP_VERSION "\n");
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }

    for (const Command *c = commands; c->name; c++) {
        if (strcmp(cmd, c->name) == 0) {
            int lock_fd = -1;
            if (c->needs_lock && fap_acquire_lock(&lock_fd) < 0) {
                fprintf(stderr, "error: %s\n", fap_err);
                return 1;
            }

            /* set FAP_TIME=1 to print how long the command took */
            int timing = getenv("FAP_TIME") != NULL;
            struct timespec t0, t1;
            if (timing)
                clock_gettime(CLOCK_MONOTONIC, &t0);

            /* Pass sub-argv: argv[0]=program, argv[1]=subcommand args */
            int ret = c->fn(argc - 1, argv + 1);

            if (timing) {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                          + (t1.tv_nsec - t0.tv_nsec) / 1e6;
                printf("%s: %.0fms\n", cmd, ms);
            }

            if (c->needs_lock)
                fap_release_lock(lock_fd);

            if (ret < 0)
                fprintf(stderr, "error: %s\n", fap_err);
            return ret < 0 ? 1 : 0;
        }
    }

    fprintf(stderr, "fap: unknown command '%s'\n\n", cmd);
    usage();
    return 1;
}
