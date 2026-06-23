#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#include "include/zephyr_buffer.h"
#include "include/zephyr_threadpool.h"
#include "include/zephyr_http_parser.h"
#include "include/zephyr_epoll_core.h"
#include "include/zephyr_log.h"
#include "include/zephyr_timer.h"

#define PORT                8080
#define MAX_EVENTS          1024
#define BACKLOG             128

/* 连接超时（毫秒） */
#define CONN_TIMEOUT_MS     30000   /* 新连接 30 秒内必须发完请求 */
#define KEEPALIVE_IDLE_MS   10000   /* keep-alive 空闲 10 秒后踢出 */

/* 全局定时器管理器 */
static zephyr_timer_mgr_t* g_timer_mgr = NULL;

/* 全局标志：信号安全退出 */
static volatile int g_server_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    g_server_running = 0;
}

/* ===================================================================
 * 定时器超时回调（运行在定时器线程）
 *
 * 只做两件轻量操作：
 *   1. 标记 timed_out = 1（worker 线程会检查此标志）
 *   2. shutdown(fd, SHUT_RDWR) 触发 EPOLLRDHUP（主循环会清理）
 *
 * 不在本线程内释放内存，避免和 worker / 主循环产生竞态。
 * =================================================================== */
static void on_connection_timeout(void* arg)
{
    zephyr_event_t* ev = (zephyr_event_t*)arg;
    if (ev == NULL) return;

    ev->timed_out = 1;
    shutdown(ev->fd, SHUT_RDWR);

    ZLOG_WARN("Connection fd=%d timed out, shutdown issued", ev->fd);
}

/* ===================================================================
 * HTTP 业务处理回调（线程池 worker 执行）
 *
 * 核心职责：
 *   1. 解析 HTTP 请求
 *   2. 组装 HTTP 响应（含文件读取）
 *   3. 发送响应数据
 *   4. 根据结果决定：re-arm（keep-alive / 续发）或关闭连接
 *
 * 注意：本函数运行在 worker 线程中，通过 EPOLLONESHOT 保证不会
 *       有两个 worker 同时操作同一个 ev。
 * =================================================================== */
static void http_business_handler(void* arg)
{
    zephyr_event_t* ev = (zephyr_event_t*)arg;
    if (ev == NULL) return;

    /* 连接已超时——定时器线程已调用 shutdown，主循环会清理 */
    if (ev->timed_out) return;

    /* ---- 1. 解析 HTTP 请求 ---- */
    zephyr_http_request_t req;
    zephyr_http_init_request(&req);

    zephyr_http_state_t state = zephyr_http_parse_request(&req, ev->input_buffer);

    if (state != PARSE_PARSE_DONE) {
        zephyr_epoll_reactivate(ev->epoll_fd, ev->fd, ev, 0);
        return;
    }

    /* 收到完整请求 → 取消连接超时定时器 */
    if (ev->timer_id > 0) {
        zephyr_timer_cancel(g_timer_mgr, ev->timer_id);
        ev->timer_id = 0;
    }

    ZLOG_INFO("Thread %ld: %s %s (keep-alive=%d)",
              (long)pthread_self(), req.method, req.url, req.keep_alive);

    /* ---- 2. 组装 HTTP 响应（含文件读取、404 等） ---- */
    int rc = zephyr_http_make_response(&req, ev->output_buffer);
    ev->keep_alive = req.keep_alive; /* 同步长短连接标志 */

    if (rc < 0) {
        /* 响应组装失败（如 realloc 失败），回 500 */
        ZLOG_ERROR("Failed to build response");
    }

    /* ---- 3. 发送响应 ---- */
    ssize_t n = zephyr_buf_write_fd(ev->output_buffer, ev->fd);

    /* ---- 4. 决定连接命运 ---- */
    if (n >= 0) {
        /* 检查是否全部发送完毕 */
        ssize_t remaining = zephyr_buf_readable_bytes(ev->output_buffer);

        if (remaining == 0) {
            /* 全部发送成功 */
            if (ev->keep_alive) {
                /* 设空闲超时：10 秒内没新请求就踢 */
                ev->timer_id = zephyr_timer_add(g_timer_mgr,
                                                KEEPALIVE_IDLE_MS, 0,
                                                on_connection_timeout, ev);
                zephyr_epoll_reactivate(ev->epoll_fd, ev->fd, ev, 0);
                ZLOG_DEBUG("Keep-alive: fd=%d re-armed", ev->fd);
            } else {
                zephyr_epoll_del(ev->epoll_fd, ev->fd);
                zephyr_buf_destroy(ev->input_buffer);
                zephyr_buf_destroy(ev->output_buffer);
                free(ev);
                ZLOG_DEBUG("Connection closed: fd cleaned");
            }
        } else {
            /* 部分发送：注册 EPOLLOUT 等待内核缓冲区腾出空间续发 */
            ev->pending_output = 1;
            zephyr_epoll_reactivate(ev->epoll_fd, ev->fd, ev, EPOLLOUT);
            ZLOG_DEBUG("Partial send (%ld bytes remaining), EPOLLOUT armed",
                       (long)remaining);
        }
    } else if (n == -2) {
        /* 内核发送缓冲区满，注册 EPOLLOUT 等待可写 */
        ev->pending_output = 1;
        zephyr_epoll_reactivate(ev->epoll_fd, ev->fd, ev, EPOLLOUT);
    } else {
        /* 致命错误（RST / 对端断开） */
        ZLOG_WARN("Write fatal error on fd=%d", ev->fd);
        zephyr_epoll_del(ev->epoll_fd, ev->fd);
        zephyr_buf_destroy(ev->input_buffer);
        zephyr_buf_destroy(ev->output_buffer);
        free(ev);
    }
}

/* ===================================================================
 * 创建监听 socket
 * =================================================================== */
static int create_listen_socket(void)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        ZLOG_FATAL("socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ZLOG_FATAL("bind() failed: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, BACKLOG) < 0) {
        ZLOG_FATAL("listen() failed: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

/* ===================================================================
 * 辅助：安全关闭一个客户端连接
 * =================================================================== */
static void cleanup_client(int epoll_fd, zephyr_event_t* ev)
{
    if (ev == NULL) return;

    /* 取消关联的定时器（0 表示无定时器，cancel 会返回 -1 但无害） */
    if (ev->timer_id > 0) {
        zephyr_timer_cancel(g_timer_mgr, ev->timer_id);
        ev->timer_id = 0;
    }

    zephyr_epoll_del(epoll_fd, ev->fd);
    zephyr_buf_destroy(ev->input_buffer);
    zephyr_buf_destroy(ev->output_buffer);
    free(ev);
}

/* ===================================================================
 * 辅助：处理 EPOLLOUT 续发
 * 返回值：1=连接已存活(re-arm), 0=连接已关闭
 * =================================================================== */
static int handle_epollout(int epoll_fd, zephyr_event_t* ev)
{
    ssize_t n = zephyr_buf_write_fd(ev->output_buffer, ev->fd);

    if (n >= 0) {
        ssize_t remaining = zephyr_buf_readable_bytes(ev->output_buffer);

        if (remaining == 0) {
            /* 数据全部发完 */
            if (ev->keep_alive) {
                ev->timer_id = zephyr_timer_add(g_timer_mgr,
                                                KEEPALIVE_IDLE_MS, 0,
                                                on_connection_timeout, ev);
                zephyr_epoll_reactivate(epoll_fd, ev->fd, ev, 0);
                return 1;
            } else {
                cleanup_client(epoll_fd, ev);
                return 0;
            }
        } else {
            /* 仍有剩余，继续等 EPOLLOUT */
            zephyr_epoll_reactivate(epoll_fd, ev->fd, ev, EPOLLOUT);
            return 1;
        }
    } else if (n == -2) {
        /* 仍需等待 */
        zephyr_epoll_reactivate(epoll_fd, ev->fd, ev, EPOLLOUT);
        return 1;
    } else {
        cleanup_client(epoll_fd, ev);
        return 0;
    }
}

/* ===================================================================
 * 主函数：Reactor 事件大循环
 * =================================================================== */
int main(void)
{
    setbuf(stdout, NULL);  /* 禁用缓冲，确保 printf 立即输出 */
    signal(SIGINT,  handle_sigint);
    signal(SIGPIPE, SIG_IGN);//忽略管道信号

    /* ---- 0. 初始化日志客户端（连接守护进程） ---- */
    zlog_init("/tmp/zephyr_logd.sock");

    ZLOG_INFO("Zephyr Server booting up...");

    /* ---- 0.5. 初始化定时器管理器 ---- */
    g_timer_mgr = zephyr_timer_create();
    if (g_timer_mgr == NULL) {
        ZLOG_FATAL("Failed to create timer manager");
        zlog_close();
        return -1;
    }
    ZLOG_INFO("Timer subsystem initialized (max %d timers)", 256);

    /* ---- 1. 初始化线程池 ---- */
    pool_t* pool = NULL;
    if (pool_init(&pool, 1000) < 0) {
        ZLOG_FATAL("Failed to initialize threadpool");
        zlog_close();
        return -1;
    }
    ZLOG_INFO("Threadpool: max %d workers, queue capacity 1000", MAXJOB);

    /* ---- 2. 创建 epoll 实例 ---- */
    int epoll_fd = zephyr_epoll_create();
    if (epoll_fd < 0) {
        ZLOG_FATAL("Failed to create epoll instance");
        pool_destroy(pool);
        zlog_close();
        return -1;
    }
    ZLOG_INFO("Epoll engine created (fd=%d)", epoll_fd);

    /* ---- 3. 开启监听 socket ---- */
    int listen_fd = create_listen_socket();
    if (listen_fd < 0) {
        ZLOG_FATAL("Failed to create listen socket");
        close(epoll_fd);
        pool_destroy(pool);
        zlog_close();
        return -1;
    }
    ZLOG_INFO("Listening on port %d (fd=%d)", PORT, listen_fd);

    /* ---- 4. 将 listen_fd 挂上 epoll（注意：listen_fd 不用 EPOLLONESHOT）---- */
    zephyr_event_t* listen_ev = malloc(sizeof(zephyr_event_t));
    listen_ev->fd               = listen_fd;
    listen_ev->epoll_fd         = epoll_fd;
    listen_ev->input_buffer     = NULL;
    listen_ev->output_buffer    = NULL;
    listen_ev->handler_callback = NULL;

    /* listen fd 手动设为非阻塞 */
    {
        int flags = fcntl(listen_fd, F_GETFL, 0);
        if (flags != -1) fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* listen fd 手动挂载（不用 EPOLLONESHOT，因为 accept 在主循环中） */
    {
        struct epoll_event ee;
        ee.events   = EPOLLIN | EPOLLET;
        ee.data.ptr = listen_ev;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ee) < 0) {
            ZLOG_FATAL("Failed to add listen_fd to epoll: %s", strerror(errno));
            close(listen_fd);
            close(epoll_fd);
            pool_destroy(pool);
            free(listen_ev);
            zlog_close();
            return -1;
        }
    }

    struct epoll_event events[MAX_EVENTS];

    ZLOG_INFO("All engines engaged. Entering Reactor Loop...");

    /* ================================================================
     * 5. Reactor 主循环
     * ================================================================ */
    while (g_server_running) {

        int nready = zephyr_epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nready; i++) {

            zephyr_event_t* ev = (zephyr_event_t*)events[i].data.ptr;
            int active_fd      = ev->fd;

            /* ---- 情况 A：新连接叩门 ---- */
            if (active_fd == listen_fd) {
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                                           (struct sockaddr*)&client_addr,
                                           &client_len);

                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        ZLOG_ERROR("accept() failed: %s", strerror(errno));
                        break;
                    }

                    ZLOG_INFO("New client: %s:%d (fd=%d)",
                              inet_ntoa(client_addr.sin_addr),
                              ntohs(client_addr.sin_port), client_fd);

                    /* 创建客户端专属全家桶 */
                    zephyr_event_t* client_ev = malloc(sizeof(zephyr_event_t));
                    client_ev->fd               = client_fd;
                    client_ev->epoll_fd         = epoll_fd;
                    client_ev->input_buffer     = zephyr_buf_create(1024);
                    client_ev->output_buffer    = zephyr_buf_create(1024);
                    client_ev->handler_callback = http_business_handler;
                    client_ev->keep_alive       = 0;
                    client_ev->pending_output   = 0;
                    client_ev->timer_id         = 0;
                    client_ev->timed_out        = 0;
                    client_ev->arg              = NULL;

                    /* 挂上 epoll（含 EPOLLONESHOT） */
                    zephyr_epoll_add(epoll_fd, client_fd, client_ev);

                    /* 设连接超时：30 秒内必须发完请求 */
                    client_ev->timer_id = zephyr_timer_add(g_timer_mgr,
                                                           CONN_TIMEOUT_MS, 0,
                                                           on_connection_timeout,
                                                           client_ev);
                }
                continue;
            }

            /* ---- 情况 B：客户端事件 ---- */
            uint32_t ev_mask = events[i].events;

            /*
             * 优先级：Error > Hangup > EPOLLOUT > EPOLLIN
             * 先检查异常，避免在已损坏的连接上做无用功。
             */
            if (ev_mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                ZLOG_INFO("Client fd=%d disconnected (events=0x%x)",
                          active_fd, ev_mask);
                cleanup_client(epoll_fd, ev);
            }
            else if (ev_mask & EPOLLOUT) {
                /* 续发此前未完成的数据 */
                handle_epollout(epoll_fd, ev);
            }
            else if (ev_mask & EPOLLIN) {
                /* 网络有数据灌入，主线程负责读取 */
                int save_errno = 0;
                ssize_t n = zephyr_buf_read_fd(ev->input_buffer, active_fd,
                                               &save_errno);

                if (n > 0) {
                    /* 数据成功吸入 → 投递到线程池 */
                    task_t task;
                    task.job = ev->handler_callback;
                    task.arg = ev;
                    pool_add_task(pool, &task);
                }
                else if (n == 0 || (n < 0 && save_errno != EAGAIN)) {
                    /* 对端关闭或恶性错误 */
                    ZLOG_WARN("Client fd=%d read error (n=%ld, errno=%d)",
                              active_fd, (long)n, save_errno);
                    cleanup_client(epoll_fd, ev);
                }
                /* n < 0 && EAGAIN: 理论上 ET + ONESHOT 不会到这，安全忽略 */
            }
        }
    }

    /* ================================================================
     * 6. 安全退出
     * ================================================================ */
    ZLOG_INFO("Shutting down...");

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listen_fd, NULL);
    close(listen_fd);
    free(listen_ev);
    close(epoll_fd);

    /* 先销毁线程池（等 worker 干完活），再销毁定时器 */
    pool_destroy(pool);
    zephyr_timer_destroy(g_timer_mgr);

    ZLOG_INFO("Goodbye.");
    zlog_close();
    return 0;
}
