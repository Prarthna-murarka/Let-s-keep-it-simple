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

/* Return empty, -, and * as NULL, so users can use that
 * to bind the server to the wildcard address.
*/
char *wildcard(char *address)
{
    /* NULL? */
    if (!address)
        return NULL;

    /* Empty? */
    if (!address[0])
        return NULL;

    /* - or ? or * or : */
    if (address[0] == '-' || address[0] == '?' ||
        address[0] == '*' || address[0] == ':')
        return NULL;

    return address;
}


int main(int argc, char *argv[])
{
    struct addrinfo         hints;
    struct addrinfo        *list, *curr;

    int             listenfd, failure;

    struct sockaddr_un     worker;
    int             workerfd, workerpathlen;

    struct sockaddr_in6     conn;
    socklen_t         connlen;
    struct msghdr         connhdr;
    struct iovec         conniov;
    struct cmsghdr        *connmsg;
    char             conndata[1];
    char             connbuf[CMSG_SPACE(sizeof (int))];
    int             connfd;

    int             result;
    ssize_t             written;

    if (argc != 4) {
        fprintf(stderr, "\n");
        fprintf(stderr, "Usage: %s ADDRESS PORT WORKER\n", argv[0]);
        fprintf(stderr, "This creates a server that binds to ADDRESS and PORT,\n");
        fprintf(stderr, "and passes each connection to a separate unrelated\n");
        fprintf(stderr, "process using an Unix domain socket at WORKER.\n");
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

    /* Unix domain socket to the worker */
    memset(&worker, 0, sizeof worker);
    worker.sun_family = AF_UNIX;

    workerpathlen = strlen(argv[3]);
    if (workerpathlen < 1) {
        fprintf(stderr, "Worker Unix domain socket path cannot be empty.\n");
        return 1;
    } else
    if (workerpathlen >= UNIX_PATH_LEN) {
        fprintf(stderr, "%s: Worker Unix domain socket path is too long.\n", argv[3]);
        return 1;
    }

    memcpy(&worker.sun_path, argv[3], workerpathlen);
    /* Note: the terminating NUL byte was set by memset(&worker, 0, sizeof worker) above. */

    workerfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (workerfd == -1) {
        fprintf(stderr, "Cannot create an Unix domain socket: %s.\n", strerror(errno));
        return 1;
    }
    if (connect(workerfd, (const struct sockaddr *)(&worker), (socklen_t)sizeof worker) == -1) {
        fprintf(stderr, "Cannot connect to %s: %s.\n", argv[3], strerror(errno));
        close(workerfd);
        return 1;
    }

    /* Initialize the address info hints */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;        /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;    /* Stream socket */
    hints.ai_flags = AI_PASSIVE        /* Wildcard ADDRESS */
                   | AI_ADDRCONFIG          /* Only return IPv4/IPv6 if available locally */
                   | AI_NUMERICSERV        /* Port must be a number */
                   ;
    hints.ai_protocol = 0;            /* Any protocol */

    /* Obtain the chain of possible addresses and ports to bind to */
    result = getaddrinfo(wildcard(argv[1]), argv[2], &hints, &list);
    if (result) {
        fprintf(stderr, "%s %s: %s.\n", argv[1], argv[2], gai_strerror(result));
        close(workerfd);
        return 1;
    }

    /* Bind to the first working entry in the chain */
    listenfd = -1;
    failure = EINVAL;
    for (curr = list; curr != NULL; curr = curr->ai_next) {
        listenfd = socket(curr->ai_family, curr->ai_socktype, curr->ai_protocol);
        if (listenfd == -1)
            continue;

        if (bind(listenfd, curr->ai_addr, curr->ai_addrlen) == -1) {
            if (!failure)
                failure = errno;
            close(listenfd);
            listenfd = -1;
            continue;
        }

        /* Bind successfully */
        break;
    }

    /* Discard the chain, as we don't need it anymore.
     * Note: curr is no longer valid after this. */
    freeaddrinfo(list);

    /* Failed to bind? */
    if (listenfd == -1) {
        fprintf(stderr, "Cannot bind to %s port %s: %s.\n", argv[1], argv[2], strerror(failure));
        close(workerfd);
        return 1;
    }

    if (listen(listenfd, LISTEN_BACKLOG) == -1) {
        fprintf(stderr, "Cannot listen for incoming connections to %s port %s: %s.\n", argv[1], argv[2], strerror(errno));
        close(listenfd);
        close(workerfd);
        return 1;
    }

    printf("Now waiting for incoming connections to %s port %s\n", argv[1], argv[2]);
    fflush(stdout);

    while (!done) {

        memset(&conn, 0, sizeof conn);
        connlen = sizeof conn;

        connfd = accept(listenfd, (struct sockaddr *)&conn, &connlen);
        if (connfd == -1) {

            /* Did we just receive a signal? */
            if (errno == EINTR)
                continue;

            /* Report a connection failure. */
            printf("Failed to accept a connection: %s\n", strerror(errno));
            fflush(stdout);

            continue;
        }

        /* Construct the message to the worker process. */
        memset(&connhdr, 0, sizeof connhdr);
        memset(&conniov, 0, sizeof conniov);
        memset(&connbuf, 0, sizeof connbuf);

        conniov.iov_base = conndata;    /* Data payload to send */
        conniov.iov_len  = 1;        /* We send just one (dummy) byte, */
        conndata[0] = 0;        /* a zero. */

        /* Construct the message (header) */
        connhdr.msg_name       = NULL;        /* No optional address */
        connhdr.msg_namelen    = 0;        /* No optional address */
        connhdr.msg_iov        = &conniov;    /* Normal payload - at least one byte */
        connhdr.msg_iovlen     = 1;        /* Only one vector in conniov */
        connhdr.msg_control    = connbuf;    /* Ancillary data */
        connhdr.msg_controllen = sizeof connbuf;

        /* Construct the ancillary data needed to pass one descriptor. */
        connmsg = CMSG_FIRSTHDR(&connhdr);
        connmsg->cmsg_level = SOL_SOCKET;
        connmsg->cmsg_type = SCM_RIGHTS;
        connmsg->cmsg_len = CMSG_LEN(sizeof (int));
        /* Copy the descriptor to the ancillary data. */
        memcpy(CMSG_DATA(connmsg), &connfd, sizeof (int));

        /* Update the message to reflect the ancillary data length */
        connhdr.msg_controllen = connmsg->cmsg_len;

        do {
            written = sendmsg(workerfd, &connhdr, MSG_NOSIGNAL);
        } while (written == (ssize_t)-1 && errno == EINTR);
        if (written == (ssize_t)-1) {
            const char *const errmsg = strerror(errno);

            /* Lost connection to the other end? */
            if (!done) {
                if (errno == EPIPE)
                    done = SIGPIPE;
                else
                    done = -1;
            }

            printf("Cannot pass connection to worker: %s.\n", errmsg);
            fflush(stdout);

            close(connfd);

            /* Break main loop. */
            break;
        }

        /* Since the descriptor has been transferred to the other process,
         * we can close our end. */
        do {
            result = close(connfd);
        } while (result == -1 && errno == EINTR);
        if (result == -1)
            printf("Error closing leftover connection descriptor: %s.\n", strerror(errno));

        printf("Connection transferred to the worker process.\n");
        fflush(stdout);
    }

    /* Shutdown. */

    close(listenfd);
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