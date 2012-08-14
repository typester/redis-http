#include <stdio.h>

#include <ev.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <sys/uio.h>

#include "hiredis.h"
#include "async.h"
#include "adapters/libev.h"

#include "ngx-queue.h"
#include "buffer.h"
#include "picohttpparser.h"

typedef struct http_server_s http_server_t;
typedef struct http_conn_s http_conn_t;

struct http_server_s {
    int fd;
    ngx_queue_t connections;
    ev_io ev_read;

    void* data;
};

static const int HTTP_CONN_ERR        = 1 << 0;
static const int HTTP_CONN_WAIT_REDIS = 1 << 1;

struct http_conn_s {
    int fd;
    ngx_queue_t queue;
    ev_io ev_read;
    ev_io ev_write;
    buffer* rbuf;

    int flags;

    http_server_t* server;
};

static http_conn_t* http_conn_init(int fd);
static void http_conn_close(http_conn_t* conn);

static const char* const BAD_REQUEST =
    "HTTP/1.0 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Bad Request";
static const size_t BAD_REQUEST_LEN = 104;

static const char* const OK_HDR =
    "HTTP/1.0 200 OK\r\n"
    "Connection: close\r\n";
static const size_t OK_HDR_LEN = 36;

static void redis_connect_cb(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        fprintf(stderr, "redis connect error: %s\n", c->errstr);
        return;
    }
    fprintf(stderr, "connected to redis\n");
}

static void redis_disconnect_cb(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        fprintf(stderr, "redis error: %d %s\n", status, c->errstr);
        return;
    }
    fprintf(stderr, "disconnected from redis\n");
}

static void redis_data_cb(redisAsyncContext* c, void* r, void* privdata) {
    redisReply* reply = r;
    http_conn_t* conn = (http_conn_t*)privdata;

    conn->flags = conn->flags ^ HTTP_CONN_WAIT_REDIS;

    if (conn->flags & HTTP_CONN_ERR) {
        http_conn_close(conn);
        return;
    }

    if (reply == NULL) {
        return;
    }

    if (conn == NULL) {
        fprintf(stderr, "invalid privdata\n");
        return;
    }

    struct iovec v[3];
    v[0].iov_base = (char*)OK_HDR;
    v[0].iov_len  = OK_HDR_LEN;

    char content_length[64];
    snprintf(content_length, 64, "Content-Length: %d\r\n\r\n", reply->len);
    v[1].iov_base = content_length;
    v[1].iov_len  = strlen(content_length);

    v[2].iov_base = reply->str;
    v[2].iov_len  = reply->len;

    writev(conn->fd, v, 3);

    http_conn_close(conn);
}

static void setup_sock(int fd) {
    int on = 1, r;

    r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    assert(r == 0);
    r = fcntl(fd, F_SETFL, O_NONBLOCK);
    assert(r == 0);
}

static void http_conn_read_cb(EV_P_ ev_io* w, int revents) {
    http_conn_t* conn = (http_conn_t*)
        (((char*)w) - offsetof(http_conn_t, ev_read));
    //http_server_t* server = conn->server;

    char buf[1024];
    ssize_t r = read(w->fd, buf, 1024);

    if (0 == r) {
        /* connection closed by peer */
#ifdef DEBUG
        fprintf(stderr, "connection closed by peer: %d\n", w->fd);
#endif
        conn->flags = conn->flags | HTTP_CONN_ERR;
        http_conn_close(conn);
        return;
    }
    else if (-1 == r) {
        /* error */
        if (EAGAIN == errno || EWOULDBLOCK == errno) { /* try again later */
            return;
        }
        else { /* fatal error */
#ifdef DEBUG
            fprintf(stderr, "fatal error: %d, %s\n", errno, strerror(errno));
#endif
            conn->flags = conn->flags | HTTP_CONN_ERR;
            http_conn_close(conn);
            return;
        }
    }
    else { /* got some data */
        buffer_append_string_len(conn->rbuf, buf, r);

        const char* method;
        const char* path;
        size_t method_len, path_len;
        int minor_version;
        size_t num_headers = 20;
        struct phr_header headers[num_headers];

        r = phr_parse_request(conn->rbuf->ptr, conn->rbuf->used, &method, &method_len,
            &path, &path_len, &minor_version, headers, &num_headers, 0);

        if (r >= 0) {
            if (0 == strncmp(method, "GET", method_len) && path_len > 1) {
                redisAsyncContext* c = (redisAsyncContext*)conn->server->data;
                redisAsyncCommand(c, redis_data_cb, conn, "GET %b", path + 1, path_len - 1);
                conn->flags = conn->flags | HTTP_CONN_WAIT_REDIS;
            }
            else {
                write(conn->fd, BAD_REQUEST, strlen(BAD_REQUEST));
                conn->flags = conn->flags | HTTP_CONN_ERR;
                http_conn_close(conn);
            }
        }
        else if (-2 == r) {
            /* partial */
            return;
        }
        else if (-1 == r) {
            write(conn->fd, BAD_REQUEST, strlen(BAD_REQUEST));
            conn->flags = conn->flags | HTTP_CONN_ERR;
            http_conn_close(conn);
        }
    }
}

static void http_acccept_cb(EV_P_ ev_io* w, int revents) {
    int newfd = accept(w->fd, NULL, NULL);
    if (0 >= newfd) {
        fprintf(stderr, "accept failed: %d, %s\n", errno, strerror(errno));
        return;
    }

    setup_sock(newfd);

    http_server_t* server = (http_server_t*)
        (((char*)w) - offsetof(http_server_t, ev_read));

    http_conn_t* conn = http_conn_init(newfd);
    ngx_queue_insert_tail(&server->connections, &conn->queue);
    conn->server = server;

    ev_io_init(&conn->ev_read, http_conn_read_cb, newfd, EV_READ);
    ev_io_start(EV_A_ &conn->ev_read);

#ifdef DEBUG
    fprintf(stderr, "new connection: %d\n", newfd);
#endif
}

static http_server_t* http_server_init() {
    http_server_t* server = malloc(sizeof(http_server_t));
    assert(server);

    server->fd = 0;
    ngx_queue_init(&server->connections);

    return server;
}

static void http_server_free(http_server_t* server) {
    free(server);
}

static http_conn_t* http_conn_init(int fd) {
    http_conn_t* conn = malloc(sizeof(http_conn_t));
    assert(conn);

    conn->fd = fd;
    ngx_queue_init(&conn->queue);
    conn->rbuf  = buffer_init();
    conn->flags = 0;

    return conn;
}

static void http_conn_close(http_conn_t* conn) {
    ev_io_stop(EV_DEFAULT_ &conn->ev_read);

    if (conn->flags & HTTP_CONN_WAIT_REDIS) return;
#ifdef DEBUG
    fprintf(stderr, "close conn: %d\n", conn->fd);
#endif
    ngx_queue_remove(&conn->queue);
    close(conn->fd);
    buffer_free(conn->rbuf);
    free(conn);
}

static void http_server_listen(http_server_t* server, uint16_t port) {
    int listen_sock, r, flag = 1;

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(-1 != listen_sock);

    r = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    assert(0 == r);

    struct sockaddr_in listen_addr;
    listen_addr.sin_family      = AF_INET;
    listen_addr.sin_port        = htons(port);
    listen_addr.sin_addr.s_addr = 0; /* ANY */
    r = bind(listen_sock, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
    assert(0 == r);

    r = listen(listen_sock, 128);
    assert(0 == r);

    setup_sock(listen_sock);

    server->fd = listen_sock;
    ev_io_init(&server->ev_read, http_acccept_cb, listen_sock, EV_READ);
    ev_io_start(EV_DEFAULT_ &server->ev_read);
}

int main() {
    /* redis client */
    redisAsyncContext* c = redisAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        fprintf(stderr, "Error: %s\n", c->errstr);
        return -1;
    }

    redisLibevAttach(EV_DEFAULT_ c);
    redisAsyncSetConnectCallback(c, redis_connect_cb);
    redisAsyncSetDisconnectCallback(c, redis_disconnect_cb);

    /* http server */
    http_server_t* server = http_server_init();
    assert(server);

    http_server_listen(server, 9999);

    server->data = (void*)c;

    /* main loop */
    ev_loop(EV_DEFAULT_ 0);

    http_server_free(server);

    return 0;
}