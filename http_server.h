#ifndef KHTTPD_HTTP_SERVER_H
#define KHTTPD_HTTP_SERVER_H

#include <linux/cpumask.h>
#include <net/sock.h>

#define RECV_BUFFER_SIZE 4096
extern mempool_t *http_buf_pool;

struct http_server_param {
    struct socket *listen_socket;
};

extern struct workqueue_struct *khttpd_wq;
/* cpumask used to pin the daemon and workqueue workers to a subset of cores.
 * Set by main.c based on the num_cores module parameter (0 = all CPUs). */
extern cpumask_var_t khttpd_cpumask;
extern atomic_t khttpd_stopped;
extern void khttpd_shutdown_all_sockets(void);
extern int init_http_routes(void);
extern void cleanup_http_routes(void);
extern int http_server_daemon(void *arg);

static inline void *http_buf_alloc(gfp_t gfp_mask, void *pool_data)
{
    return kzalloc(RECV_BUFFER_SIZE, gfp_mask);
}

static inline void http_buf_free(void *element, void *pool_data)
{
    kfree(element);
}
#endif
