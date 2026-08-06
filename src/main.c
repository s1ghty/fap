#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
} Command;

static const Command commands[] = {
    { "install",  cmd_install  },
    { "remove",   cmd_remove   },
    { "sync",     cmd_sync     },
    { "update",   cmd_update   },
    { "search",   cmd_search   },
    { "list",     cmd_list     },
    { "info",     cmd_info     },
    { "channels", cmd_channels },
    { NULL, NULL }
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
            /* Pass sub-argv: argv[0]=program, argv[1]=subcommand args */
            int ret = c->fn(argc - 1, argv + 1);
            if (ret < 0)
                fprintf(stderr, "error: %s\n", fap_err);
            return ret < 0 ? 1 : 0;
        }
    }

    fprintf(stderr, "fap: unknown command '%s'\n\n", cmd);
    usage();
    return 1;
}
