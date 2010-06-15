/* TODO: remove dependence on gnu */
/* - don't use asprintf */
/* - don't use getline */
/* - don't use O_CLOEXEC :( */
#define _GNU_SOURCE             /* getline, O_CLOEXEC, asprintf */

/* standard library */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* posix */
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* other */
#include <alloca.h>             /* alloca */
#include <sys/file.h>           /* flock */

/* constants */
#define QUEUE_FILENAME  "queue"
#define LOCK_FILENAME   "lock"
#define JOBS_DIRNAME    "jobs"
#define READ_BUF_SIZE   512

/* failure modes */
#define DIE_IF(name, exp) ((exp) ? die(#name) : (void) 0)
#define ORDIE(name, exp) DIE_IF(name, !(exp))
#define CHECK(name, exp) DIE_IF(name, (exp) < 0)
#define CHECK_EOF(name, exp) DIE_IF(name, (exp) == EOF)
#define ENSURE(func, ...) CHECK(#func, func(__VA_ARGS__))
#define ENSURE_EOF(func, ...) CHECK_EOF(#func, func(__VA_ARGS__))

/* debugging */
#define DEBUG(fmt, ...) (fprintf(stderr, fmt, ## __VA_ARGS__))
#define SAY(fmt, ...) DEBUG(fmt "\n", ## __VA_ARGS__)

/* globals */
int lock_fd;
char *cqdir;

void die(const char *s) {
    perror(s);
    exit(1);
}

void lock(int fd) { ENSURE(flock, fd, LOCK_EX);  }
void unlock(int fd) { ENSURE(flock, fd, LOCK_UN); }

void fcopy(FILE *write, FILE *read) {
    char buf[READ_BUF_SIZE];
    char *s;
    for (;;) {
        s = fgets(buf, READ_BUF_SIZE, read);
        if (!s) {
            int why = errno;
            if (feof(read)) break; /* EOF. */
            errno = why;
            die("fgets");
        }

        ENSURE_EOF(fputs, buf, write);
    }

    /* Truncate the write file & we're done. */
    fflush(write);
    long off = ftell(write);
    ENSURE(ftruncate, fileno(write), off);
}

char *getjob(char *path) {
    FILE *readf = fopen(path, "r");
    ORDIE(fopen, readf);

    /* Get a line from the file. */
    size_t n;
    char *line = NULL;
    ssize_t len = getline(&line, &n, readf);
    CHECK(getline, len);

    /* Remove that line from the file. */
    FILE *writef = fopen(path, "w+");
    ORDIE(fopen, writef);
    fcopy(writef, readf);
    ENSURE_EOF(fclose, writef);
    ENSURE_EOF(fclose, readf);

    /* Remove the trailing newline. */
    if (line[len-1] == '\n')
        line[len-1] = '\0';
    return line;
}

void child_runjob(char *jobdir, char *job) {
    char *runfile;
    ENSURE(asprintf, &runfile, "%s/run", jobdir);

    char *screenname;
    ENSURE(asprintf, &screenname, "cq.%s", job);

    SAY("running: %s", runfile);

    /* Redirect stdin, stdout to /dev/null. */
    int fd = open("/dev/null", O_CLOEXEC);
    (void) fd;
    CHECK(open, fd);
    ENSURE(dup2, fd, 0);
    ENSURE(dup2, fd, 1);

    /* Run it in a screen. */
    execlp("screen", "screen",
           "-D", "-m",       /* start detached, but don't fork a new process. */
           "-S", screenname, /* give it a useful session name */
           "--", runfile,    /* run the given job */
           NULL);
    die("execlp");
}

void runjob(char *job) {
    char *jobdir;
    ENSURE(asprintf, &jobdir, "%s/%s/%s", cqdir, JOBS_DIRNAME, job);

    /* Acquire a lock on the job. */
    int jobdirfd = open(jobdir, O_CLOEXEC);
    CHECK(open, jobdirfd);
    lock(jobdirfd);

    pid_t pid = fork();
    if (!pid) child_runjob(jobdir, job); /* We are the child. */

    /* Write the pid to the pidfile. */
    char *pidfilename;
    ENSURE(asprintf, &pidfilename, "%s/pid", jobdir);

    FILE *pidf = fopen(pidfilename, "w");
    ORDIE(fopen, pidf);
    free(pidfilename);

    ENSURE(fprintf, pidf, "%d\n", pid);
    ENSURE_EOF(fflush, pidf);
    ENSURE_EOF(fclose, pidf);

    /* Wait for our child to die. */
    int status = -1;
    SAY("waiting for child %d, job %s", pid, job);
    ENSURE(wait, &status);

    /* Log result. */
    if (status)
        ENSURE(fprintf, stderr, "job %s failed with status %d\n",
               job, status);
    else
        ENSURE(fprintf, stderr, "job %s succeeded\n", job);

    /* TODO: cleanup the job (remove job file & directory). */
    unlock(jobdirfd);
    ENSURE(close, jobdirfd);
}

void nop_handler(int i) { (void) i; }

int main(int argc, char **argv) {
    /* Mask out SIGUSR1 and give it a nop action. */
    sigset_t sset_new, sset_old;
    ENSURE(sigemptyset, &sset_new);
    ENSURE(sigaddset, &sset_new, SIGUSR1);
    ENSURE(sigprocmask, SIG_BLOCK, &sset_new, &sset_old);

    struct sigaction sact = { .sa_handler = nop_handler };
    ENSURE(sigaction, SIGUSR1, &sact, NULL);

    /* Determine paths. */
    cqdir = getenv("CQDIR");
    size_t cqdirlen = strlen(cqdir);
    /* we add 2, 1 for the slash, and 1 for the null terminator. */
    char *queue_path = alloca(cqdirlen + strlen(QUEUE_FILENAME) + 2);
    char *lock_path = alloca(cqdirlen + strlen(LOCK_FILENAME) + 2);
    sprintf(queue_path, "%s/%s", cqdir, QUEUE_FILENAME);
    sprintf(lock_path, "%s/%s", cqdir, LOCK_FILENAME);

    /* Open lock file. */
    lock_fd = open(lock_path, O_CREAT | O_CLOEXEC,
                   S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    CHECK(lock_fd, open);

    /* Event loop. */
    for (;;) {
        for (;;) {
            lock(lock_fd);

            /* See whether the queue is empty. */
            struct stat stbuf;
            int err = lstat(queue_path, &stbuf);

            /* If the file does not exist or is empty, we wait. */
            if ((err < 0 && errno == ENOENT) || (!err && !stbuf.st_size))
                break;

            CHECK(err, lstat);

            /* The queue exists and is nonempty. Get a job from it. */
            char *line = getjob(queue_path);
            SAY("got job: %s", line);

            /* Unlock and execute that job. */
            unlock(lock_fd);
            SAY("running job: %s", line);
            runjob(line);
            free(line);
        }
        unlock(lock_fd);

        /* Wait to be woken up. */
        SAY("waiting for SIGUSR1");
        sigsuspend(&sset_old);
        ORDIE(sigsuspend, errno == EINTR);
        SAY("woke up");
    }
}
