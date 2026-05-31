#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <sys/queue.h>
#include <sched.h>

#define PORT        9000
#define DATAFILE    "/var/tmp/aesdsocketdata"

volatile sig_atomic_t caught_sigint  = 0;
volatile sig_atomic_t caught_sigterm = 0;

pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

void signal_handler(int signal_number)
{
    if (signal_number == SIGINT)  caught_sigint  = 1;
    if (signal_number == SIGTERM) caught_sigterm = 1;
}

/* ── thread node ─────────────────────────────────────────── */

typedef struct thread_node {
    pthread_t           thread;
    int                 client_fd;
    struct sockaddr_in  cli_addr;
    bool                done;
    SLIST_ENTRY(thread_node) entries;
} thread_node_t;

SLIST_HEAD(thread_list, thread_node) thread_head = SLIST_HEAD_INITIALIZER(thread_head);

/* ── worker thread ───────────────────────────────────────── */

void *connection_handler(void *arg)
{
    thread_node_t *node = (thread_node_t *)arg;
    int fd = node->client_fd;
    unsigned char *ip = (unsigned char *)&node->cli_addr.sin_addr.s_addr;

    /* Receive until newline or connection closed */
    size_t buf_size = 256, buf_used = 0;
    char *dynbuf = malloc(buf_size);
    if (!dynbuf) { syslog(LOG_ERR, "malloc failed"); goto done; }

    while (1) {
        if (buf_used == buf_size) {
            buf_size *= 2;
            char *tmp = realloc(dynbuf, buf_size);
            if (!tmp) { free(dynbuf); syslog(LOG_ERR, "realloc failed"); goto done; }
            dynbuf = tmp;
        }
        ssize_t n = read(fd, dynbuf + buf_used, buf_size - buf_used);
        if (n < 0) { free(dynbuf); syslog(LOG_ERR, "read error"); goto done; }
        if (n == 0) break;
        buf_used += n;
        if (memchr(dynbuf, '\n', buf_used)) break;
    }

    /* Append to file under mutex */
    pthread_mutex_lock(&file_mutex);
    int wfd = open(DATAFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (wfd < 0) {
        pthread_mutex_unlock(&file_mutex);
        free(dynbuf);
        syslog(LOG_ERR, "open for write failed");
        goto done;
    }
    ssize_t written = write(wfd, dynbuf, buf_used);
    close(wfd);
    pthread_mutex_unlock(&file_mutex);
    free(dynbuf);

    if (written < 0) { syslog(LOG_ERR, "write failed"); goto done; }

    /* Send full file back - no mutex held during send */
    int rfd = open(DATAFILE, O_RDONLY);
    if (rfd < 0) {
        syslog(LOG_ERR, "open for read failed");
        goto done;
    }
    char filebuf[256];
    ssize_t bytes;
    while ((bytes = read(rfd, filebuf, sizeof(filebuf))) > 0) {
        if (write(fd, filebuf, bytes) < 0) {
            close(rfd);
            syslog(LOG_ERR, "send failed");
            goto done;
        }
    }
    close(rfd);

    syslog(LOG_DEBUG, "Closed connection from %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

done:
    close(fd);
    node->done = true;
    return NULL;
}

/* ── timer thread ────────────────────────────────────────── */

void *timer_handler(void *arg)
{
    (void)arg;
    while (!caught_sigint && !caught_sigterm) {
        sleep(10);
        if (caught_sigint || caught_sigterm) break;

        time_t t = time(NULL);
        struct tm *tm_info = localtime(&t);
        char tsbuf[128];
        strftime(tsbuf, sizeof(tsbuf), "timestamp:%a, %d %b %Y %T %z\n", tm_info);

        pthread_mutex_lock(&file_mutex);
        int wfd = open(DATAFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (wfd >= 0) {
            write(wfd, tsbuf, strlen(tsbuf));
            close(wfd);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

/* ── main ────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    struct sigaction new_action;
    memset(&new_action, 0, sizeof(new_action));
    new_action.sa_handler = signal_handler;
    sigaction(SIGTERM, &new_action, NULL);
    sigaction(SIGINT,  &new_action, NULL);

    openlog(argv[0], LOG_PID, LOG_USER);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port        = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    listen(sockfd, 5);

    /* Daemonise if -d */
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        pid_t pid = fork();
        if (pid < 0) error("ERROR on fork");
        if (pid > 0) exit(0);
        umask(0);
        if (setsid() < 0) error("ERROR on setsid");
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    /* Start timer thread */
    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, timer_handler, NULL) != 0)
        error("ERROR creating timer thread");

    /* Accept loop */
    while (!caught_sigint && !caught_sigterm) {
        struct sockaddr_in cli_addr;
        socklen_t clilen = sizeof(cli_addr);
        int newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (newsockfd < 0) {
            if (errno == EINTR) break;
            error("ERROR on accept");
        }

        unsigned char *ip = (unsigned char *)&cli_addr.sin_addr.s_addr;
        syslog(LOG_DEBUG, "Accepted connection from %d.%d.%d.%d",
               ip[0], ip[1], ip[2], ip[3]);

        /* Allocate and populate thread node */
        thread_node_t *node = malloc(sizeof(thread_node_t));
        if (!node) error("ERROR malloc thread node");
        node->client_fd = newsockfd;
        node->cli_addr  = cli_addr;
        node->done      = false;
        SLIST_INSERT_HEAD(&thread_head, node, entries);

        if (pthread_create(&node->thread, NULL, connection_handler, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            SLIST_REMOVE_HEAD(&thread_head, entries);
            close(newsockfd);
            free(node);
            continue;
        }

        /* Yield to let the new thread get scheduled before we loop back */
        sched_yield();

        /* Reap completed threads */
        thread_node_t *cur, *tmp;
        cur = SLIST_FIRST(&thread_head);
        while (cur != NULL) {
            tmp = SLIST_NEXT(cur, entries);
            if (cur->done) {
                pthread_join(cur->thread, NULL);
                SLIST_REMOVE(&thread_head, cur, thread_node, entries);
                free(cur);
            }
            cur = tmp;
        }
    }

    /* Shutdown: join timer then all workers */
    pthread_join(timer_thread, NULL);

    thread_node_t *cur, *tmp;
    cur = SLIST_FIRST(&thread_head);
    while (cur != NULL) {
        tmp = SLIST_NEXT(cur, entries);
        pthread_join(cur->thread, NULL);
        SLIST_REMOVE(&thread_head, cur, thread_node, entries);
        free(cur);
        cur = tmp;
    }

    close(sockfd);
    remove(DATAFILE);
    syslog(LOG_DEBUG, "Caught signal, exiting");
    closelog();
    pthread_mutex_destroy(&file_mutex);
    return 0;
}