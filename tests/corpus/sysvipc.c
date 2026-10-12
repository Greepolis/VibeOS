/* A producer and a consumer over System V shared memory and semaphores, and a
 * round trip through each kind of message queue (docs/abi/ L7 step 6).
 *
 * Two processes share a ring of sixteen slots in a segment. Three semaphores
 * guard it - free slots, full slots, and the ring itself - and SEM_UNDO on the
 * last, so a process that died holding it would not leave it held. The
 * producer writes two thousand numbers, the consumer adds them up; then the
 * same numbers go both ways through a SysV queue by type and a POSIX queue by
 * priority. Every line is a fact about what arrived, so the output is the same
 * on Linux and here, and the corpus compares the two.
 *
 * Built against musl statically and against glibc dynamically (corpus-build.sh). */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOTS 16
#define ITEMS 2000

enum { FREE, FULL, RING };

struct ring {
    unsigned head, tail;
    uint64_t slot[SLOTS];
};

static int sem_do(int set, unsigned short num, short op, short flg) {
    struct sembuf b = {num, op, flg};
    int r;

    while ((r = semop(set, &b, 1)) != 0 && errno == EINTR) {
    }
    return r;
}

static void die(const char *what) {
    printf("sysvipc: %s failed: %s\n", what, strerror(errno));
    exit(1);
}

static int shared_ring(void) {
    int shm, set, status;
    struct ring *r;
    pid_t pid;
    uint64_t sum = 0, want = 0;
    unsigned short init[3] = {SLOTS, 0, 1};
    struct shmid_ds ds;
    int i;

    if ((shm = shmget(IPC_PRIVATE, sizeof(struct ring), IPC_CREAT | 0600)) < 0) {
        die("shmget");
    }
    if ((set = semget(IPC_PRIVATE, 3, IPC_CREAT | 0600)) < 0) {
        die("semget");
    }
    if (semctl(set, 0, SETALL, init) != 0) {
        die("semctl SETALL");
    }
    if ((r = shmat(shm, 0, 0)) == (void *)-1) {
        die("shmat");
    }
    r->head = r->tail = 0;
    if ((pid = fork()) < 0) {
        die("fork");
    }
    if (pid == 0) {
        /* The producer. */
        for (i = 1; i <= ITEMS; i++) {
            if (sem_do(set, FREE, -1, 0) || sem_do(set, RING, -1, SEM_UNDO)) {
                _exit(2);
            }
            r->slot[r->head++ % SLOTS] = (uint64_t)i * i;
            if (sem_do(set, RING, 1, SEM_UNDO) || sem_do(set, FULL, 1, 0)) {
                _exit(2);
            }
        }
        shmdt(r);
        _exit(0);
    }
    for (i = 1; i <= ITEMS; i++) {
        if (sem_do(set, FULL, -1, 0) || sem_do(set, RING, -1, SEM_UNDO)) {
            die("consumer semop");
        }
        sum += r->slot[r->tail++ % SLOTS];
        if (sem_do(set, RING, 1, SEM_UNDO) || sem_do(set, FREE, 1, 0)) {
            die("consumer semop");
        }
        want += (uint64_t)i * i;
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        die("producer");
    }
    printf("ring: %d items through %d slots, sum %llu, %s\n", ITEMS, SLOTS,
           (unsigned long long)sum, sum == want ? "as sent" : "WRONG");
    if (shmctl(shm, IPC_STAT, &ds) != 0) {
        die("shmctl IPC_STAT");
    }
    printf("ring: attached %lu after the producer left\n", (unsigned long)ds.shm_nattch);
    printf("ring: semaphores free=%d full=%d ring=%d\n", semctl(set, FREE, GETVAL), semctl(set, FULL, GETVAL),
           semctl(set, RING, GETVAL));
    shmdt(r);
    shmctl(shm, IPC_RMID, 0);
    semctl(set, 0, IPC_RMID);
    return 0;
}

struct msg {
    long type;
    char text[32];
};

static int sysv_queue(void) {
    struct msg m;
    int q, i;
    long order[5];

    if ((q = msgget(IPC_PRIVATE, IPC_CREAT | 0600)) < 0) {
        die("msgget");
    }
    for (i = 5; i >= 1; i--) {
        m.type = i;
        snprintf(m.text, sizeof(m.text), "message %d", i);
        if (msgsnd(q, &m, strlen(m.text) + 1, 0) != 0) {
            die("msgsnd");
        }
    }
    /* By type: the lowest at or below 3 first, then whatever is left. */
    for (i = 0; i < 5; i++) {
        if (msgrcv(q, &m, sizeof(m.text), i < 3 ? -3 : 0, 0) < 0) {
            die("msgrcv");
        }
        order[i] = m.type;
    }
    printf("sysv queue: received %ld %ld %ld %ld %ld\n", order[0], order[1], order[2], order[3], order[4]);
    printf("sysv queue: empty now is %s\n",
           msgrcv(q, &m, sizeof(m.text), 0, IPC_NOWAIT) < 0 && errno == ENOMSG ? "ENOMSG" : "something else");
    msgctl(q, IPC_RMID, 0);
    return 0;
}

static int posix_queue(void) {
    struct mq_attr a = {0, 8, 64, 0, {0}};
    char buf[64];
    unsigned prio, order[4];
    mqd_t q;
    int i;

    mq_unlink("/vibeos-sysvipc");
    if ((q = mq_open("/vibeos-sysvipc", O_CREAT | O_EXCL | O_RDWR, 0600, &a)) == (mqd_t)-1) {
        die("mq_open");
    }
    for (i = 0; i < 4; i++) {
        snprintf(buf, sizeof(buf), "prio %d", (i * 7) % 4);
        if (mq_send(q, buf, strlen(buf) + 1, (unsigned)((i * 7) % 4)) != 0) {
            die("mq_send");
        }
    }
    for (i = 0; i < 4; i++) {
        if (mq_receive(q, buf, sizeof(buf), &prio) < 0) {
            die("mq_receive");
        }
        order[i] = prio;
    }
    printf("posix queue: received priorities %u %u %u %u\n", order[0], order[1], order[2], order[3]);
    mq_getattr(q, &a);
    printf("posix queue: maxmsg %ld msgsize %ld curmsgs %ld\n", a.mq_maxmsg, a.mq_msgsize, a.mq_curmsgs);
    mq_close(q);
    printf("posix queue: unlink %s\n", mq_unlink("/vibeos-sysvipc") == 0 ? "ok" : strerror(errno));
    return 0;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    shared_ring();
    sysv_queue();
    posix_queue();
    return 0;
}
