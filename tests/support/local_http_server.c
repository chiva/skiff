#include "local_http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* How often the server thread checks whether it should stop. */
#define LOCAL_HTTP_POLL_MS 20
#define LOCAL_HTTP_BACKLOG 4
#define REQUEST_END "\r\n\r\n"

/* Waits until fd is readable or the server is told to stop; returns 1 when readable. */
static int wait_readable(local_http_server *server, int fd) {
    struct pollfd watched = {.fd = fd, .events = POLLIN};
    while (!atomic_load(&server->stop)) {
        const int ready = poll(&watched, 1, LOCAL_HTTP_POLL_MS);
        if (ready > 0) {
            return 1;
        }
        if (ready < 0) {
            return 0;
        }
    }
    return 0;
}

/* Reads one request (or, with on_first_bytes, whatever arrives first) into the next request slot;
 * returns 0 when the client closed the connection or the server is stopping. */
static int read_request(local_http_server *server, int fd, const local_http_reply *reply) {
    char *request = server->requests[server->request_count];
    size_t used = 0;
    while (used < LOCAL_HTTP_REQUEST_MAX - 1) {
        if (!wait_readable(server, fd)) {
            return 0;
        }
        const ssize_t got = recv(fd, request + used, LOCAL_HTTP_REQUEST_MAX - 1 - used, 0);
        if (got <= 0) {
            return 0;
        }
        used += (size_t)got;
        request[used] = '\0';
        if (reply->on_first_bytes || strstr(request, REQUEST_END) != NULL) {
            break;
        }
    }
    server->request_count++;
    return 1;
}

static void send_all(int fd, const char *bytes, size_t size) {
    while (size > 0) {
        const ssize_t sent = send(fd, bytes, size, MSG_NOSIGNAL);
        if (sent <= 0) {
            return;
        }
        bytes += sent;
        size -= (size_t)sent;
    }
}

static void serve_connection(local_http_server *server, int fd) {
    while (server->next_reply < server->reply_count) {
        const local_http_reply *reply = &server->replies[server->next_reply];
        if (!read_request(server, fd, reply)) {
            return;
        }
        server->next_reply++;
        send_all(fd, reply->bytes, reply->size);
        if (reply->after == LOCAL_HTTP_CLOSE) {
            return;
        }
        if (reply->after == LOCAL_HTTP_STALL) {
            while (!atomic_load(&server->stop)) {
                poll(NULL, 0, LOCAL_HTTP_POLL_MS);
            }
            return;
        }
    }
}

static void *serve(void *arg) {
    local_http_server *server = arg;
    while (wait_readable(server, server->listen_fd)) {
        const int fd = accept(server->listen_fd, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        server->connections++;
        serve_connection(server, fd);
        close(fd);
    }
    return NULL;
}

/* A socket bound to 127.0.0.1 on an ephemeral port, whose number goes to *port. */
static int bind_loopback(unsigned short *port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = 0};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

int local_http_server_start(local_http_server *server, const local_http_reply *replies,
                            size_t count) {
    if (count > LOCAL_HTTP_MAX_REPLIES) {
        return -1;
    }
    memset(server, 0, sizeof *server);
    atomic_init(&server->stop, 0);
    memcpy(server->replies, replies, count * sizeof *replies);
    server->reply_count = count;
    server->listen_fd = bind_loopback(&server->port);
    if (server->listen_fd < 0) {
        return -1;
    }
    if (listen(server->listen_fd, LOCAL_HTTP_BACKLOG) != 0 ||
        pthread_create(&server->thread, NULL, serve, server) != 0) {
        close(server->listen_fd);
        return -1;
    }
    return 0;
}

void local_http_server_stop(local_http_server *server) {
    atomic_store(&server->stop, 1);
    pthread_join(server->thread, NULL);
    close(server->listen_fd);
}

unsigned short local_http_unused_port(void) {
    unsigned short port = 0;
    const int fd = bind_loopback(&port);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return port;
}
