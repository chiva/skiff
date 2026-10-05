#ifndef SKIFF_TEST_LOCAL_HTTP_SERVER_H
#define SKIFF_TEST_LOCAL_HTTP_SERVER_H

/*
 * A scripted server on 127.0.0.1 for the curl transport's hermetic tests: it answers each request
 * with the next reply from its script, byte for byte, so a test can send exactly the response (or
 * the failure) it wants: a cut-off body, a stalled connection, garbage to a TLS client. Requests
 * are recorded for assertions; read them after local_http_server_stop().
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#define LOCAL_HTTP_MAX_REPLIES 8
#define LOCAL_HTTP_REQUEST_MAX 2048

typedef enum local_http_after {
    LOCAL_HTTP_KEEP_OPEN, /* wait for the next request on the same connection */
    LOCAL_HTTP_CLOSE,     /* close the connection */
    LOCAL_HTTP_STALL,     /* keep the connection open and send nothing more */
} local_http_after;

typedef struct local_http_reply {
    const char *bytes;
    size_t size;
    local_http_after after;
    /* Reply as soon as anything arrives instead of after a full request: a TLS ClientHello has no
     * blank line. */
    int on_first_bytes;
} local_http_reply;

typedef struct local_http_server {
    int listen_fd;
    unsigned short port;
    pthread_t thread;
    atomic_int stop;
    local_http_reply replies[LOCAL_HTTP_MAX_REPLIES];
    size_t reply_count;
    size_t next_reply;
    char requests[LOCAL_HTTP_MAX_REPLIES][LOCAL_HTTP_REQUEST_MAX];
    size_t request_count;
    int connections;
} local_http_server;

/* Listens on an ephemeral port and serves replies in order. Returns 0 on success. */
int local_http_server_start(local_http_server *server, const local_http_reply *replies,
                            size_t count);

/* Stops serving, closes every socket and joins the thread. */
void local_http_server_stop(local_http_server *server);

/* A port nothing listens on (bound, then closed), for connection-refused tests. 0 on failure. */
unsigned short local_http_unused_port(void);

#endif
