#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mempool.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <net/sock.h>

#include "http_server.h"

#define DEFAULT_PORT 9000
#define DEFAULT_BACKLOG 65535
#define POOL_MIN_NR 4
#define ACCEPT_TIMEOUT_MS 5000

mempool_t *http_buf_pool;
struct workqueue_struct *khttpd_wq;
cpumask_var_t khttpd_cpumask;

static ushort port = DEFAULT_PORT;
module_param(port, ushort, S_IRUGO);
static ushort backlog = DEFAULT_BACKLOG;
module_param(backlog, ushort, S_IRUGO);
/* num_cores: number of CPU cores to pin khttpd to (0 = all cores, default).
 * Example: insmod khttpd.ko num_cores=2 pins to cpu0 and cpu1. */
static uint num_cores = 0;
module_param(num_cores, uint, S_IRUGO);

static struct socket *listen_socket;
static struct http_server_param param;
static struct task_struct *http_server;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
static int set_sock_opt(struct socket *sock,
                        int level,
                        int optname,
                        char *optval,
                        unsigned int optlen)
{
    int ret = 0;

    if (optlen < sizeof(int))
        return -EINVAL;

    switch (optname) {
    case SO_REUSEADDR:
        sock_set_reuseaddr(sock->sk);
        break;
    case SO_RCVBUF:
        sock_set_rcvbuf(sock->sk, *(int *) optval);
        break;
    }

    return ret;
}

static int set_tcp_opt(struct socket *sock,
                       int level,
                       int optname,
                       char *optval,
                       unsigned int optlen)
{
    int ret = 0;

    if (optlen < sizeof(int))
        return -EINVAL;

    switch (optname) {
    case TCP_NODELAY:
        tcp_sock_set_nodelay(sock->sk);
        break;
    case TCP_CORK:
        tcp_sock_set_cork(sock->sk, *(bool *) optval);
        break;
    }

    return ret;
}

static int kernel_setsockopt(struct socket *sock,
                             int level,
                             int optname,
                             char *optval,
                             unsigned int optlen)
{
    if (level == SOL_SOCKET)
        return set_sock_opt(sock, level, optname, optval, optlen);
    else if (level == SOL_TCP)
        return set_tcp_opt(sock, level, optname, optval, optlen);
    return -EINVAL;
}
#endif

static inline int setsockopt(struct socket *sock,
                             int level,
                             int optname,
                             int optval)
{
    int opt = optval;
    return kernel_setsockopt(sock, level, optname, (char *) &opt, sizeof(opt));
}

static int open_listen_socket(ushort port, ushort backlog, struct socket **res)
{
    struct socket *sock;
    struct sockaddr_in s;

    int err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
    if (err < 0) {
        pr_err("sock_create() failure, err=%d\n", err);
        return err;
    }

    err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
    if (err < 0)
        goto bail_setsockopt;

    err = setsockopt(sock, SOL_TCP, TCP_NODELAY, 1);
    if (err < 0)
        goto bail_setsockopt;

    err = setsockopt(sock, SOL_TCP, TCP_CORK, 0);
    if (err < 0)
        goto bail_setsockopt;

    err = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, 1024 * 1024);
    if (err < 0)
        goto bail_setsockopt;

    err = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, 1024 * 1024);
    if (err < 0)
        goto bail_setsockopt;

    memset(&s, 0, sizeof(s));
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_ANY);
    s.sin_port = htons(port);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
    err = kernel_bind(sock, (struct sockaddr_unsized *) &s, sizeof(s));
#else
    err = kernel_bind(sock, (struct sockaddr *) &s, sizeof(s));
#endif
    if (err < 0) {
        pr_err("kernel_bind() failure, err=%d\n", err);
        goto bail_sock;
    }

    /* Let accept() time out so the daemon can reap finished workers */
    WRITE_ONCE(sock->sk->sk_rcvtimeo, msecs_to_jiffies(ACCEPT_TIMEOUT_MS));

    err = kernel_listen(sock, backlog);
    if (err < 0) {
        pr_err("kernel_listen() failure, err=%d\n", err);
        goto bail_sock;
    }
    *res = sock;
    return 0;

bail_setsockopt:
    pr_err("kernel_setsockopt() failure, err=%d\n", err);
bail_sock:
    sock_release(sock);
    return err;
}

static void close_listen_socket(struct socket *socket)
{
    kernel_sock_shutdown(socket, SHUT_RDWR);
    sock_release(socket);
}

static int __init khttpd_init(void)
{
    int err;
    uint cpu, pinned = 0;

    /* Build the CPU affinity mask for the daemon + workqueue workers.
     * num_cores=0 means "all CPUs" (leave mask empty → no pinning).
     * num_cores=N pins to the first N online CPUs (cpu0, cpu1, …, cpuN-1). */
    if (!zalloc_cpumask_var(&khttpd_cpumask, GFP_KERNEL)) {
        pr_err("failed to allocate cpumask\n");
        return -ENOMEM;
    }
    if (num_cores > 0) {
        for_each_online_cpu(cpu) {
            cpumask_set_cpu(cpu, khttpd_cpumask);
            if (++pinned >= num_cores)
                break;
        }
        pr_info("CPU affinity: pinned to %u core(s) (%*pbl)\n",
                pinned, cpumask_pr_args(khttpd_cpumask));
    } else {
        pr_info("CPU affinity: unrestricted (num_cores=0)\n");
    }

    err = init_http_routes();
    if (err < 0) {
        pr_err("failed to initialize http routes\n");
        return err;
    }

    if (!(http_buf_pool = mempool_create(POOL_MIN_NR, http_buf_alloc,
                                         http_buf_free, NULL))) {
        pr_err("failed to create mempool\n");
        cleanup_http_routes();
        err = -ENOMEM;
        goto err_free_cpumask;
    }

    khttpd_wq = alloc_workqueue("khttpd_wq", WQ_HIGHPRI | WQ_MEM_RECLAIM, 0);
    if (!khttpd_wq) {
        pr_err("failed to create khttpd workqueue\n");
        err = -ENOMEM;
        goto err_destroy_pool;
    }

    err = open_listen_socket(port, backlog, &listen_socket);
    if (err < 0) {
        pr_err("can't open listen socket\n");
        goto err_destroy_wq;
    }
    param.listen_socket = listen_socket;
    http_server = kthread_run(http_server_daemon, &param, KBUILD_MODNAME);
    if (IS_ERR(http_server)) {
        pr_err("can't start http server daemon\n");
        err = PTR_ERR(http_server);
        goto err_close_socket;
    }
    return 0;

err_close_socket:
    close_listen_socket(listen_socket);
err_destroy_wq:
    destroy_workqueue(khttpd_wq);
    khttpd_wq = NULL;
err_destroy_pool:
    mempool_destroy(http_buf_pool);
    http_buf_pool = NULL;
    cleanup_http_routes();
err_free_cpumask:
    free_cpumask_var(khttpd_cpumask);
    return err;
}

static void __exit khttpd_exit(void)
{
    send_sig(SIGTERM, http_server, 1);
    kthread_stop(http_server);
    close_listen_socket(listen_socket);
    if (khttpd_wq) {
        destroy_workqueue(khttpd_wq);
        khttpd_wq = NULL;
    }
    mempool_destroy(http_buf_pool);
    cleanup_http_routes();
    free_cpumask_var(khttpd_cpumask);
    pr_info("module unloaded\n");
}

module_init(khttpd_init);
module_exit(khttpd_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("in-kernel HTTP daemon");
MODULE_VERSION("0.1");
