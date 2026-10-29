#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
#define kthread_complete_and_exit(comp, code) complete_and_exit(comp, code)
#endif

#include "http_parser.h"
#include "http_server.h"

#define CRLF "\r\n"

#define HTTP_RESPONSE_200_DUMMY                               \
    ""                                                        \
    "HTTP/1.1 200 OK" CRLF "Server: " KBUILD_MODNAME CRLF     \
    "Content-Type: text/plain" CRLF "Content-Length: 12" CRLF \
    "Connection: Close" CRLF CRLF "Hello World!" CRLF
#define HTTP_RESPONSE_200_KEEPALIVE_DUMMY                     \
    ""                                                        \
    "HTTP/1.1 200 OK" CRLF "Server: " KBUILD_MODNAME CRLF     \
    "Content-Type: text/plain" CRLF "Content-Length: 12" CRLF \
    "Connection: Keep-Alive" CRLF CRLF "Hello World!" CRLF
#define HTTP_RESPONSE_501                                              \
    ""                                                                 \
    "HTTP/1.1 501 Not Implemented" CRLF "Server: " KBUILD_MODNAME CRLF \
    "Content-Type: text/plain" CRLF "Content-Length: 21" CRLF          \
    "Connection: Close" CRLF CRLF "501 Not Implemented" CRLF
#define HTTP_RESPONSE_501_KEEPALIVE                                    \
    ""                                                                 \
    "HTTP/1.1 501 Not Implemented" CRLF "Server: " KBUILD_MODNAME CRLF \
    "Content-Type: text/plain" CRLF "Content-Length: 21" CRLF          \
    "Connection: KeepAlive" CRLF CRLF "501 Not Implemented" CRLF


struct http_request {
    struct socket *socket;
    enum http_method method;
    char request_url[128];
    int complete;
};

/* Work structure for each accepted connection dispatched to khttpd_wq */
struct khttpd_work {
    struct work_struct work;
    struct socket *socket;
};

static int http_server_recv(struct socket *sock, char *buf, size_t size)
{
    struct kvec iov = {.iov_base = (void *) buf, .iov_len = size};
    struct msghdr msg = {
        .msg_name = 0,
        .msg_namelen = 0,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0,
    };
    return kernel_recvmsg(sock, &msg, &iov, 1, size, msg.msg_flags);
}

static int http_server_send(struct socket *sock, const char *buf, size_t size)
{
    struct msghdr msg = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0,
    };
    int done = 0;
    while (done < size) {
        struct kvec iov = {
            .iov_base = (void *) ((char *) buf + done),
            .iov_len = size - done,
        };
        int length = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
        if (length < 0) {
            /* Silently drop disconnection errors (EPIPE, ECONNRESET) in
             * the fast-path to avoid pr_err latency spikes in dmesg. */
            pr_debug("write error: %d\n", length);
            break;
        }
        done += length;
    }
    return done;
}

struct http_route {
    const char *url;
    char *resp_keepalive;
    size_t resp_keepalive_len;
    char *resp_close;
    size_t resp_close_len;
};

struct route_def {
    const char *url;
    const char *body;
    size_t body_len;
};

static const struct route_def default_routes[] = {
    {"/teste",          "Hello, world!", 13},
    {"/16b",            NULL,            16},
    {"/64b",            NULL,            64},
    {"/256b",           NULL,           256},
    {"/512b",           NULL,           512},
    {"/1k",             NULL,          1000},
    {"/unaccelerated",  NULL,          1000},
    {"/passthrough",    NULL,          1000},
    {"/2k",             NULL,          2000},
    {"/4k",             NULL,          4000},
    {"/large",          NULL,          4000},
    {"/8k",             NULL,          8000},
    {"/16k",            NULL,         16000},
    {"/32k",            NULL,         32000},
    {"/64k",            NULL,         64000},
    {"/128k",           NULL,        128000},
    {"/256k",           NULL,        256000},
};
#define NUM_ROUTES ARRAY_SIZE(default_routes)

static struct http_route parsed_routes[NUM_ROUTES];
static struct http_route fallback_route;

static int build_http_response(const char *body,
                               size_t body_len,
                               int keep_alive,
                               char **out_buf,
                               size_t *out_len)
{
    char hdr[256];
    int hdr_len;
    size_t total_len;
    char *buf;

    hdr_len = snprintf(hdr, sizeof(hdr),
                       "HTTP/1.1 200 OK\r\n"
                       "Server: " KBUILD_MODNAME "\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: %s\r\n\r\n",
                       body_len, keep_alive ? "Keep-Alive" : "Close");
    if (hdr_len < 0 || (size_t) hdr_len >= sizeof(hdr))
        return -EINVAL;

    total_len = (size_t) hdr_len + body_len;
    buf = kvmalloc(total_len, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    memcpy(buf, hdr, hdr_len);
    if (body)
        memcpy(buf + hdr_len, body, body_len);
    else
        memset(buf + hdr_len, 'X', body_len);

    *out_buf = buf;
    *out_len = total_len;
    return 0;
}

int init_http_routes(void)
{
    size_t i;
    int ret;

    for (i = 0; i < NUM_ROUTES; i++) {
        parsed_routes[i].url = default_routes[i].url;
        ret = build_http_response(default_routes[i].body, default_routes[i].body_len,
                                  1, &parsed_routes[i].resp_keepalive,
                                  &parsed_routes[i].resp_keepalive_len);
        if (ret < 0)
            goto err_free;

        ret = build_http_response(default_routes[i].body, default_routes[i].body_len,
                                  0, &parsed_routes[i].resp_close,
                                  &parsed_routes[i].resp_close_len);
        if (ret < 0)
            goto err_free;
    }

    fallback_route.url = "*";
    ret = build_http_response("Hello from khttpd!\r\n", 19, 1,
                              &fallback_route.resp_keepalive,
                              &fallback_route.resp_keepalive_len);
    if (ret < 0)
        goto err_free;

    ret = build_http_response("Hello from khttpd!\r\n", 19, 0,
                              &fallback_route.resp_close,
                              &fallback_route.resp_close_len);
    if (ret < 0)
        goto err_free;

    pr_info("Registered %zu benchmark payload routes\n", NUM_ROUTES);
    return 0;

err_free:
    cleanup_http_routes();
    return ret;
}

void cleanup_http_routes(void)
{
    size_t i;

    for (i = 0; i < NUM_ROUTES; i++) {
        kvfree(parsed_routes[i].resp_keepalive);
        parsed_routes[i].resp_keepalive = NULL;
        parsed_routes[i].resp_keepalive_len = 0;

        kvfree(parsed_routes[i].resp_close);
        parsed_routes[i].resp_close = NULL;
        parsed_routes[i].resp_close_len = 0;
    }

    kvfree(fallback_route.resp_keepalive);
    fallback_route.resp_keepalive = NULL;
    fallback_route.resp_keepalive_len = 0;

    kvfree(fallback_route.resp_close);
    fallback_route.resp_close = NULL;
    fallback_route.resp_close_len = 0;
}

static int http_server_response(struct http_request *request, int keep_alive)
{
    const struct http_route *route = NULL;
    size_t i;

    if (request->method != HTTP_GET) {
        const char *resp = keep_alive ? HTTP_RESPONSE_501_KEEPALIVE : HTTP_RESPONSE_501;
        return http_server_send(request->socket, resp, strlen(resp));
    }

    for (i = 0; i < NUM_ROUTES; i++) {
        if (strcmp(request->request_url, parsed_routes[i].url) == 0) {
            route = &parsed_routes[i];
            break;
        }
    }

    if (!route)
        route = &fallback_route;

    if (keep_alive)
        return http_server_send(request->socket, route->resp_keepalive, route->resp_keepalive_len);
    else
        return http_server_send(request->socket, route->resp_close, route->resp_close_len);
}

static int http_parser_callback_message_begin(http_parser *parser)
{
    struct http_request *request = parser->data;
    struct socket *socket = request->socket;
    memset(request, 0x00, sizeof(struct http_request));
    request->socket = socket;
    return 0;
}

static int http_parser_callback_request_url(http_parser *parser,
                                            const char *p,
                                            size_t len)
{
    struct http_request *request = parser->data;
    size_t cur_len = strlen(request->request_url);
    if (cur_len + len < sizeof(request->request_url)) {
        memcpy(request->request_url + cur_len, p, len);
        request->request_url[cur_len + len] = '\0';
    }
    return 0;
}

static int http_parser_callback_header_field(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_header_value(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_headers_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    request->method = parser->method;
    return 0;
}

static int http_parser_callback_body(http_parser *parser,
                                     const char *p,
                                     size_t len)
{
    return 0;
}

static int http_parser_callback_message_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    http_server_response(request, http_should_keep_alive(parser));
    request->complete = 1;
    return 0;
}

static void http_server_worker_fn(struct work_struct *work)
{
    char *buf;
    struct http_parser parser;
    struct http_parser_settings setting = {
        .on_message_begin = http_parser_callback_message_begin,
        .on_url = http_parser_callback_request_url,
        .on_header_field = http_parser_callback_header_field,
        .on_header_value = http_parser_callback_header_value,
        .on_headers_complete = http_parser_callback_headers_complete,
        .on_body = http_parser_callback_body,
        .on_message_complete = http_parser_callback_message_complete,
    };
    struct http_request request;
    struct khttpd_work *kw = container_of(work, struct khttpd_work, work);
    struct socket *socket = kw->socket;

    /* Pin this worker kthread to the configured core set (same mask used
     * by the daemon thread).  Keeps khttpd bounded to the same CPUs as
     * Nginx / xhttp in the same benchmark round. */
    if (!cpumask_empty(khttpd_cpumask))
        set_cpus_allowed_ptr(current, khttpd_cpumask);

    buf = mempool_alloc(http_buf_pool, GFP_KERNEL);
    if (!buf) {
        pr_err("can't allocate memory for worker buffer!\n");
        goto out;
    }

    request.socket = socket;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = &request;
    while (!kthread_should_stop()) {
        int ret = http_server_recv(socket, buf, RECV_BUFFER_SIZE - 1);
        if (ret <= 0)
            goto out_free_buf;

        http_parser_execute(&parser, &setting, buf, ret);
        if (request.complete && !http_should_keep_alive(&parser))
            goto out_free_buf;
        memset(buf, 0, RECV_BUFFER_SIZE);
    }

out_free_buf:
    mempool_free(buf, http_buf_pool);
out:
    kernel_sock_shutdown(socket, SHUT_RDWR);
    sock_release(socket);
    kfree(kw);
}

int http_server_daemon(void *arg)
{
    struct socket *socket;
    struct khttpd_work *kw;
    struct http_server_param *param = (struct http_server_param *) arg;

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    /* Pin this daemon thread (and indirectly the bound workqueue workers
     * that will run on the same CPUs) to the configured core set. */
    if (!cpumask_empty(khttpd_cpumask))
        set_cpus_allowed_ptr(current, khttpd_cpumask);

    while (!kthread_should_stop()) {
        int err;

        err = kernel_accept(param->listen_socket, &socket, 0);
        if (err < 0) {
            if (signal_pending(current))
                break;
            /* -EAGAIN means the accept timeout expired; loop back */
            if (err != -EAGAIN)
                pr_err("kernel_accept() error: %d\n", err);
            continue;
        }
        /* Accepted sockets inherit the listener's timeout. Restore the
         * default so idle keep-alive connections are not dropped.
         */
        WRITE_ONCE(socket->sk->sk_rcvtimeo, MAX_SCHEDULE_TIMEOUT);

        /* Disable Nagle algorithm on accepted client socket to eliminate delayed ACKs */
        tcp_sock_set_nodelay(socket->sk);

        /* Expand send/recv buffers on the accepted socket to 1 MB so that
         * large payloads (>= 32 KB) can fill the TCP window without stalling.
         * The listen socket already has these set, but accepted sockets
         * inherit the system default (usually 87 KB), not the listen socket's
         * value, so we must set them explicitly here.
         */
        sock_set_rcvbuf(socket->sk, 1024 * 1024);
        WRITE_ONCE(socket->sk->sk_sndbuf, 2 * 1024 * 1024);

        kw = kmalloc(sizeof(*kw), GFP_KERNEL);
        if (!kw) {
            pr_err("can't allocate memory for connection work\n");
            kernel_sock_shutdown(socket, SHUT_RDWR);
            sock_release(socket);
            continue;
        }
        kw->socket = socket;
        INIT_WORK(&kw->work, http_server_worker_fn);
        queue_work(khttpd_wq, &kw->work);
    }

    return 0;
}
