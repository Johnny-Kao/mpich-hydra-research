/* MPICH Hydra research: PMI2 lifecycle control / fault test.
 * This is intentionally research-only; it must not be sent upstream as-is.
 */
#define _POSIX_C_SOURCE 200809L
#include <pmi2.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void delay_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
}

static const char *state_dir(void)
{
    const char *dir = getenv("PMI_RESEARCH_DIR");
    if (!dir || !*dir) {
        fprintf(stderr, "PMI_RESEARCH_DIR missing\n");
        exit(2);
    }
    return dir;
}

static void state_file(char *buf, size_t cap)
{
    if (snprintf(buf, cap, "%s/waiter.pid", state_dir()) >= (int) cap)
        exit(2);
}

static void mark_waiter(void)
{
    char path[512];
    state_file(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fprintf(f, "%ld\n", (long) getpid());
    fclose(f);
}

static pid_t find_waiter(void)
{
    char path[512];
    state_file(path, sizeof(path));
    for (int i = 0; i < 150; i++) {
        FILE *f = fopen(path, "r");
        if (f) {
            long pid = -1;
            int rc = fscanf(f, "%ld", &pid);
            fclose(f);
            if (rc == 1 && pid > 1) return (pid_t) pid;
        }
        delay_ms(100);
    }
    fprintf(stderr, "waiter.pid not published\n");
    exit(3);
}

/* Confirm Hydra has queued the requests before killing a waiting rank. */
static void wait_for_queued(int second_waiter)
{
    char path[512];
    if (snprintf(path, sizeof(path), "%s/queued.log", state_dir()) >=
        (int) sizeof(path))
        exit(18);

    for (int i = 0; i < 200; i++) {
        int seen0 = 0, seen2 = !second_waiter;
        FILE *f = fopen(path, "r");
        if (f) {
            int rank;
            while (fscanf(f, "%d", &rank) == 1) {
                if (rank == 0) seen0 = 1;
                if (rank == 2) seen2 = 1;
            }
            fclose(f);
        }
        if (seen0 && seen2) {
            printf("QUEUED_CONFIRMED ranks=%s\n",
                   second_waiter ? "0,2" : "0");
            return;
        }
        delay_ms(100);
    }
    fprintf(stderr, "Queued PMI2 requests were not confirmed before kill\n");
    exit(19);
}

static void *blocking_get(void *arg)
{
    (void) arg;
    char value[128] = {0};
    int found = 0;
    int rc = PMI2_Info_GetNodeAttr("mpich_research_attr", value, sizeof(value),
                                   &found, 1);
    printf("WAITER_GET rc=%d found=%d value=%s\n", rc, found, value);
    fflush(stdout);
    return (void *)(intptr_t)(rc != PMI2_SUCCESS || !found ||
                                strcmp(value, "ready") != 0);
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "control") && strcmp(argv[1], "fault") && strcmp(argv[1], "fault-no-wait") && strcmp(argv[1], "fault-two-waiters"))) {
        fprintf(stderr, "usage: %s control|fault|fault-no-wait|fault-two-waiters\n", argv[0]); return 2;
    }
    signal(SIGUSR1, SIG_IGN);

    int spawned = -1, size = 0, rank = -1, appnum = -1;
    int rc = PMI2_Init(&spawned, &size, &rank, &appnum);
    if (rc != PMI2_SUCCESS) {
        fprintf(stderr, "PMI2_Init rc=%d\n", rc); return 3;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("START mode=%s rank=%d size=%d pid=%ld\n",
           argv[1], rank, size, (long)getpid());

    if (!strcmp(argv[1], "control")) {
        if (size != 2) return 4;
        if (rank == 0) {
            char value[128] = {0};
            int found = 0;
            rc = PMI2_Info_GetNodeAttr("mpich_research_attr", value,
                                       sizeof(value), &found, 1);
            printf("CONTROL_GET rc=%d found=%d value=%s\n", rc, found, value);
            if (rc || !found || strcmp(value, "ready")) return 5;
        } else {
            delay_ms(700);
            rc = PMI2_Info_PutNodeAttr("mpich_research_attr", "ready");
            printf("CONTROL_PUT rc=%d\n", rc);
            if (rc) return 6;
        }
        rc = PMI2_Finalize();
        printf("CONTROL_DONE rank=%d finalize=%d\n", rank, rc);
        return rc ? 7 : 0;
    }

    if (size != 3) return 8;
    if (rank == 0) {
        if (!strcmp(argv[1], "fault-no-wait")) {
            /* Same forced process exit, but no pending node-attribute request. */
            delay_ms(1200);
            mark_waiter();
            while (1) pause();
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, blocking_get, NULL)) return 9;
        delay_ms(1200); /* allow the PMI request to enter pending queue */
        mark_waiter();
        pthread_join(tid, NULL);
        return 10; /* expected to be killed while still pending */
    }
    if (rank == 1) {
        pid_t pid = find_waiter();
        if (strcmp(argv[1], "fault-no-wait"))
            wait_for_queued(!strcmp(argv[1], "fault-two-waiters"));
        printf("KILL_WAITER pid=%ld\n", (long)pid);
        if (kill(pid, SIGKILL) != 0) { perror("kill"); return 11; }
        delay_ms(600);
        rc = PMI2_Info_PutNodeAttr("mpich_research_attr", "ready");
        printf("FAULT_PUT rc=%d\n", rc);
        if (rc) return 12;
        rc = PMI2_Finalize();
        printf("FAULT_PUT_DONE rc=%d\n", rc);
        return rc ? 13 : 0;
    }

    if (!strcmp(argv[1], "fault-two-waiters")) {
        /* This rank must remain queued after the disconnected rank's
         * request is canceled. Both wait on the same attribute key. */
        char value[128] = {0};
        int found = 0;
        printf("SURVIVING_WAITER_WAIT rank=%d\n", rank);
        rc = PMI2_Info_GetNodeAttr("mpich_research_attr", value,
                                   sizeof(value), &found, 1);
        printf("SURVIVING_WAITER_GET rc=%d found=%d value=%s\n", rc, found, value);
        if (rc || !found || strcmp(value, "ready"))
            return 16;
        rc = PMI2_Finalize();
        printf("SURVIVING_WAITER_DONE rc=%d\n", rc);
        return rc ? 17 : 0;
    }

    delay_ms(2300);
    rc = PMI2_Info_PutNodeAttr("mpich_research_survivor", "ready");
    printf("SURVIVOR_PUT rc=%d\n", rc);
    if (rc) return 14;
    rc = PMI2_Finalize();
    printf("SURVIVOR_DONE rc=%d\n", rc);
    return rc ? 15 : 0;
}
