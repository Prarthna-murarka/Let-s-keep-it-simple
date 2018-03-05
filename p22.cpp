#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* How many concurrent pending connections are allowed */
#define  LISTEN_BACKLOG     32

/* Unix domain socket path length (including NUL byte) */
#ifndef  UNIX_PATH_LEN
#define  UNIX_PATH_LEN    108
#endif

/* Flag to indicate we have received a shutdown request. */
volatile sig_atomic_t     done = 0;

/* Shutdown request signal handler, of the basic type. */
void handle_done_signal(int signum)
{
    if (!done)
        done = signum;

    return;
}

/* Install shutdown request signal handler on signal signum. */
int set_done_signal(const int signum)
{
    struct sigaction act;

    sigemptyset(&act.sa_mask);
    act.sa_handler = handle_done_signal;
    act.sa_flags = 0;

    if (sigaction(signum, &act, NULL) == -1)
        return errno;
    else
        return 0;
}

/* Helper function to duplicate file descriptors.
 * Returns 0 if success, errno error code otherwise.
*/
static int copy_fd(const int fromfd, const int tofd)
{
    int result;

    if (fromfd == tofd)
        return 0;

    if (fromfd == -1 || tofd == -1)
        return errno = EINVAL;

    do {
        result = dup2(fromfd, tofd);
    } while (result == -1 && errno == EINTR);
    if (result == -1)
        return errno;

    return 0;
}

int main(int argc, char *argv[])
{
    struct sockaddr_un     worker;
    int             workerfd, workerpathlen;
    int             serverfd, clientfd;

    pid_t             child;

    struct msghdr         msghdr;
    struct iovec         msgiov;
    struct cmsghdr        *cmsg;
    char             data[1];
    char             ancillary[CMSG_SPACE(sizeof (int))];
    ssize_t             received;

    if (argc < 3) {
        fprintf(stderr, "\n");
        fprintf(stderr, "Usage: %s WORKER COMMAND [ ARGS .. ]\n", argv[0]);
        fprintf(stderr, "This creates a worker that receives connections\n");
        fprintf(stderr, "from Unix domain socket WORKER.\n");
        fprintf(stderr, "Each connection is served by COMMAND, with the\n");
        fprintf(stderr, "connection connected to its standard input and output.\n");
        fprintf(stderr, "\n");
        return (argc == 1) ? 0 : 1;
    }

    /* Handle HUP, INT, PIPE, and TERM signals,
     * so when the user presses Ctrl-C, the worker process cannot be contacted,
     * or the user sends a HUP or TERM signal, this server closes down cleanly. */
    if (set_done_signal(SIGINT) ||
        set_done_signal(SIGHUP) ||
        set_done_signal(SIGPIPE) ||
        set_done_signal(SIGTERM)) {
        fprintf(stderr, "Error: Cannot install signal handlers.\n");
        return 1;
    }

    /* Unix domain socket */
    memset(&worker, 0, sizeof worker);
    worker.sun_family = AF_UNIX;

    workerpathlen = strlen(argv[1]);
    if (workerpathlen < 1) {
        fprintf(stderr, "Worker Unix domain socket path cannot be empty.\n");
        return 1;
    } else
    if (workerpathlen >= UNIX_PATH_LEN) {
        fprintf(stderr, "%s: Worker Unix domain socket path is too long.\n", argv[1]);
        return 1;
    }

    memcpy(&worker.sun_path, argv[1], workerpathlen);
    /* Note: the terminating NUL byte was set by memset(&worker, 0, sizeof worker) above. */

    workerfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (workerfd == -1) {
        fprintf(stderr, "Cannot create an Unix domain socket: %s.\n", strerror(errno));
        return 1;
    }
    if (bind(workerfd, (const struct sockaddr *)(&worker), (socklen_t)sizeof worker) == -1) {
        fprintf(stderr, "%s: %s.\n", argv[1], strerror(errno));
        close(workerfd);
        return 1;
    }
    if (listen(workerfd, LISTEN_BACKLOG) == -1) {
        fprintf(stderr, "%s: Cannot listen for messages: %s.\n", argv[1], strerror(errno));
        close(workerfd);
        return 1;
    }

    printf("Listening for descriptors on %s.\n", argv[1]);
    fflush(stdout);

    while (!done) {

        serverfd = accept(workerfd, NULL, NULL);
        if (serverfd == -1) {

            if (errno == EINTR)
                continue;

            printf("Failed to accept a connection from the server: %s.\n", strerror(errno));
            fflush(stdout);
            continue;
        }

        printf("Connection from the server.\n");
        fflush(stdout);

        while (!done && serverfd != -1) {

            memset(&msghdr, 0, sizeof msghdr);
            memset(&msgiov, 0, sizeof msgiov);

            msghdr.msg_name       = NULL;
            msghdr.msg_namelen    = 0;
            msghdr.msg_control    = &ancillary;
            msghdr.msg_controllen = sizeof ancillary;

            cmsg = CMSG_FIRSTHDR(&msghdr);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof (int));

            msghdr.msg_iov    = &msgiov;
            msghdr.msg_iovlen = 1;

            msgiov.iov_base    = &data;
            msgiov.iov_len = 1; /* Just one byte */

            received = recvmsg(serverfd, &msghdr, 0);

            if (received == (ssize_t)-1) {
                if (errno == EINTR)
                    continue;

                printf("Error receiving a message from server: %s.\n", strerror(errno));
                fflush(stdout);
                break;
            }

            cmsg = CMSG_FIRSTHDR(&msghdr);
            if (!cmsg || cmsg->cmsg_len != CMSG_LEN(sizeof (int))) {
                printf("Received a bad message from server.\n");
                fflush(stdout);
                break;
            }

            memcpy(&clientfd, CMSG_DATA(cmsg), sizeof (int));

            printf("Executing command with descriptor %d: ", clientfd);
            fflush(stdout);

            child = fork();
            if (child == (pid_t)-1) {
                printf("Fork failed: %s.\n", strerror(errno));
                fflush(stdout);
                close(clientfd);
                break;
            }

            if (!child) {
                /* This is the child process. */

                close(workerfd);
                close(serverfd);

                if (copy_fd(clientfd, STDIN_FILENO) ||
                    copy_fd(clientfd, STDOUT_FILENO) ||
                    copy_fd(clientfd, STDERR_FILENO))
                    return 126; /* Exits the client */

                if (clientfd != STDIN_FILENO &&
                    clientfd != STDOUT_FILENO &&
                    clientfd != STDERR_FILENO)
                    close(clientfd);

                execvp(argv[2], argv + 2);

                return 127; /* Exits the client */
            }

            printf("Done.\n");
            fflush(stdout);

            close(clientfd);
        }

        close(serverfd);

        printf("Closed connection to server.\n");
        fflush(stdout);        
    }

    /* Shutdown. */
    close(workerfd);

    switch (done) {
    case SIGTERM:
        printf("Terminated.\n");
        break;

    case SIGPIPE:
        printf("Lost connection.\n");
        break;

    case SIGHUP:
        printf("Hanging up.\n");
        break;

    case SIGINT:
        printf("Interrupted; exiting.\n");
        break;

    default:
        printf("Exiting.\n");
    }

    return 0;
}
