#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "fap.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); pass++; } \
    else       { printf("  FAIL  %s  [%s]\n", msg, fap_err); fail++; } \
} while(0)

/* fap_acquire_lock()'s two fds in this same process are still separate
 * open file descriptions (each is its own open() call) — flock()
 * associates the lock with the open file description, not the
 * process, so this genuinely exercises the same conflict a second
 * real `fap` process would hit. See util.c's fap_acquire_lock(). */
static void test_lock_conflict(void)
{
    const char *home = "/tmp/fap_test_util_home";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", home, home);
    system(cmd);
    setenv("HOME", home, 1);

    int fd1 = -1;
    CHECK(fap_acquire_lock(&fd1) == 0, "first acquire succeeds");
    CHECK(fd1 >= 0, "first acquire returns a valid fd");

    char lockpath[512];
    snprintf(lockpath, sizeof(lockpath), "%s/.local/fap/.lock", home);
    struct stat st;
    CHECK(stat(lockpath, &st) == 0, "lock guard file created under the active root");

    int fd2 = -1;
    int rc2 = fap_acquire_lock(&fd2);
    CHECK(rc2 < 0, "second acquire fails while the first is held");
    CHECK(fd2 == -1, "second acquire leaves fd_out untouched on failure");

    fap_release_lock(fd1);

    int fd3 = -1;
    CHECK(fap_acquire_lock(&fd3) == 0, "acquire succeeds again once the first is released");
    fap_release_lock(fd3);

    unsetenv("HOME");
}

int main(void)
{
    printf("util (locking):\n");
    test_lock_conflict();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
