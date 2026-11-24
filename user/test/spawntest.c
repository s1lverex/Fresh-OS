/* spawntest N - spawn and reap N processes, verify no memory leaks */
#include <stdio.h>
#include <stdlib.h>
#include <fresh.h>

static int run_batch(int n)
{
    char *argv[] = {"true", NULL};
    for (int i = 0; i < n; i++) {
        int pid = spawn("/bin/true", argv, NULL, 0);
        if (pid < 0) {
            printf("spawntest: spawn failed at %d: %s\n", i, strerror(pid));
            return 1;
        }
        int st;
        if (wait_pid(pid, &st) != pid || st != 0) {
            printf("spawntest: bad wait result at %d\n", i);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    if (run_batch(50))
        return 1;
    long before = fresh_test(TEST_FREE_PAGES, 0);
    long heap_before = fresh_test(TEST_HEAP_USED, 0);
    uint64_t t0 = uptime_ms();
    if (run_batch(n))
        return 1;
    uint64_t t1 = uptime_ms();
    long after = fresh_test(TEST_FREE_PAGES, 0);
    long heap_after = fresh_test(TEST_HEAP_USED, 0);
    /* parallel: many children alive at once */
    char *argv2[] = {"sleep", "0.2", NULL};
    int pids[40];
    int ok = 1;
    for (int i = 0; i < 40; i++) {
        pids[i] = spawn("/bin/sleep", argv2, NULL, 0);
        if (pids[i] < 0) {
            printf("spawntest: parallel spawn %d failed: %s\n", i, strerror(pids[i]));
            ok = 0;
            pids[i] = 0;
        }
    }
    for (int i = 0; i < 40; i++)
        if (pids[i]) {
            int st;
            if (wait_pid(pids[i], &st) != pids[i] || st != 0)
                ok = 0;
        }
    long after2 = fresh_test(TEST_FREE_PAGES, 0);
    printf("spawntest: %d spawns in %llu ms, page delta %ld, heap delta %ld, parallel %s, final delta %ld\n", n,
           (unsigned long long)(t1 - t0), before - after, heap_after - heap_before, ok ? "ok" : "FAILED",
           before - after2);
    int pass = ok && before == after && heap_before == heap_after && before == after2;
    printf("spawntest: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
