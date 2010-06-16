/* TODO: remove dependence on gnu/linux */
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
#include <stdarg.h>

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
#define CQDIR_ENVAR     "CQDIR"
#define CURRENT_DIR     "current"
#define JOBS_DIR        "jobs"
#define LOCK_PATH       "lock"
#define QUEUE_PATH      "queue"
#define READ_BUF_SIZE   512

/* failure modes */
#define DIE_IF(name, exp) ((exp) ? die(#name) : (void) 0)
#define ORDIE(name, exp) DIE_IF(name, !(exp))
#define CHECK(name, exp) DIE_IF(name, (exp) < 0)
#define CHECK_EOF(name, exp) DIE_IF(name, (exp) == EOF)
#define ENSURE(func, ...) CHECK(#func, func(__VA_ARGS__))
#define ENSURE_EOF(func, ...) CHECK_EOF(#func, func(__VA_ARGS__))

/* utility */
#define DEFSTRING(name, fmt, ...)               \
    char *name;                                 \
    ENSURE(asprintf, &name, fmt, __VA_ARGS__)

/* debugging */
#define DEBUG(fmt, ...) (fprintf(stderr, fmt, ## __VA_ARGS__))
#define SAY(fmt, ...) DEBUG(fmt "\n", ## __VA_ARGS__)

/* globals */
int lock_fd;
char *cq_dir;
char *jobs_dir;
char *current_dir;
char *queue_path;

void nop_handler(int i) { (void) i; } /* no-op signal handler */

void die(const char *s) {
    perror(s);
    exit(1);
}

void lock(int fd) { ENSURE(flock, fd, LOCK_EX);  }
void unlock(int fd) { ENSURE(flock, fd, LOCK_UN); }

int open_file_v(int flags, mode_t mode, const char *fmt, va_list ap) {
    char *path;
    ENSURE(vasprintf, &path, fmt, ap);
    int fd = open(path, flags, mode);
    CHECK(open, fd);
    free(path);
    return fd;
}

FILE *fopen_file_v(const char *mode, const char *fmt, va_list ap) {
    char *path;
    ENSURE(vasprintf, &path, fmt, ap);
    FILE *file = fopen(path, mode);
    ORDIE(fopen, file);
    free(path);
    return file;
}

int lock_file_v(int flags, mode_t mode, const char *fmt, va_list ap) {
    int fd = open_file_v(flags, mode, fmt, ap);
    lock(fd);
    return fd;
}

#define VA_WRAP(restype, argn, func, ...) do {          \
        va_list __ap;                                   \
        va_start(__ap, argn);                           \
        restype __result = func(__VA_ARGS__, __ap);     \
        va_end(__ap);                                   \
        return __result;                                \
    } while (0)

int open_file(int flags, mode_t mode, const char *fmt, ...)
{ VA_WRAP(int, fmt, open_file_v, flags, mode, fmt); }

FILE *fopen_file(const char *mode, const char *fmt, ...)
{ VA_WRAP(FILE*, fmt, fopen_file_v, mode, fmt); }

int lock_file(int flags, mode_t mode, const char *fmt, ...)
{ VA_WRAP(int, fmt, lock_file_v, flags, mode, fmt); }

void unlock_file(int fd) { unlock(fd); ENSURE(close, fd); }

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

char *readjob(char *path) {
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


/* Actually running the job. */
void child_runjob(char *runfile) {
    char *screenname = "cq";

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

void run_job(char *jobdir) {
    DEFSTRING(runfile, "%s/run", jobdir);
    DEFSTRING(pidfile, "%s/pid", jobdir);
    DEFSTRING(lockfile, "%s/%s", jobdir, LOCK_PATH);

    /* Fork off the job process. */
    pid_t pid = fork();
    if (!pid) child_runjob(runfile); /* We are the child. */

    /* Write the pid to the pidfile. */
    FILE *pidf = fopen(pidfile, "w");
    ORDIE(fopen, pidf);
    ENSURE(fprintf, pidf, "%d\n", pid);
    ENSURE_EOF(fclose, pidf);

    /* Wait for our child to die. */
    int status = -1;
    SAY("waiting for child %d", pid);
    ENSURE(wait, &status);

    /* Log result. */
    if (status)
        ENSURE(fprintf, stderr, "child %d failed with status %d\n",
               pid, status);
    else
        ENSURE(fprintf, stderr, "child %d succeeded\n", pid);

    /* Clean up. */
    ENSURE(unlink, pidfile);  free(pidfile);
    ENSURE(unlink, runfile);  free(runfile);
    ENSURE(unlink, lockfile); free(lockfile);
    ENSURE(rmdir, jobdir);
}


void take_job(char *queue_path) {
    /* Get a job from the queue. */
    char *job = readjob(queue_path);
    SAY("got job: %s", job);

    char *job_dir;
    ENSURE(asprintf, &job_dir, "%s/%s", jobs_dir, job);

    /* Lock the job. */
    int job_lock_fd = lock_file(O_CLOEXEC, 0,
                                "%s/%s", job_dir, LOCK_PATH);

    /* Move the job's directory. */
    ENSURE(rename, job_dir, current_dir);

    unlock_file(job_lock_fd);
    free(job_dir);
}

int main(int argc, char **argv) {
    (void) argc, (void) argv;

    /* Mask out SIGUSR1 and give it a nop action. */
    sigset_t sset_new, sset_old;
    ENSURE(sigemptyset, &sset_new);
    ENSURE(sigaddset, &sset_new, SIGUSR1);
    ENSURE(sigprocmask, SIG_BLOCK, &sset_new, &sset_old);

    struct sigaction sact = { .sa_handler = nop_handler };
    ENSURE(sigaction, SIGUSR1, &sact, NULL);

    /* Determine some paths. */
    cq_dir = getenv(CQDIR_ENVAR);
    /* FIXME: error if cq_dir is not valid. */

    ENSURE(asprintf, &queue_path, "%s/%s", cq_dir, QUEUE_PATH);
    ENSURE(asprintf, &jobs_dir, "%s/%s", cq_dir, JOBS_DIR);
    ENSURE(asprintf, &current_dir, "%s/%s", cq_dir, CURRENT_DIR);

    /* Lock the lock file. */
    lock_fd = lock_file(O_CREAT | O_CLOEXEC,
                        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH,
                        "%s/%s", cq_dir, LOCK_PATH);

    /* Event loop. */
    for (;;) {
        for (;;) {
            lock(lock_fd);

            /* See whether the queue is empty. */
            struct stat stbuf;
            int err = lstat(queue_path, &stbuf);

            /* If the queue file does not exist or is empty, we wait. */
            if ((err < 0 && errno == ENOENT) || (!err && !stbuf.st_size))
                break;

            CHECK(lstat, err);

            /* Take a job from the queue, make it current, run it. */
            take_job(queue_path);
            unlock(lock_fd);
            run_job(current_dir);
        }
        unlock(lock_fd);

        /* Wait to be woken up. */
        SAY("waiting for SIGUSR1");
        sigsuspend(&sset_old);
        ORDIE(sigsuspend, errno == EINTR);
        SAY("woke up");
    }
}
