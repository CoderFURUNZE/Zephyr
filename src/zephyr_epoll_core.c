#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include "../include/zephyr_epoll_core.h"

/*---------------------------------------------------------------------------
 * 内部辅助：将 fd 设为非阻塞
 *---------------------------------------------------------------------------*/
static int __set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*---------------------------------------------------------------------------
 * 创建 epoll 实例
 *---------------------------------------------------------------------------*/
int zephyr_epoll_create(void)
{
    int epoll_fd = epoll_create(1024);
    if (epoll_fd < 0) {
        perror("[Zephyr Epoll] epoll_create() failed");
        return -1;
    }
    return epoll_fd;
}

/*---------------------------------------------------------------------------
 * 向 epoll 红黑树添加监听节点
 *
 * 核心策略：
 *   EPOLLIN      — 监听可读数据
 *   EPOLLET      — 边缘触发，不榨干不重复通知
 *   EPOLLRDHUP   — 对端半关闭时立即感知（FIN 包）
 *   EPOLLONESHOT — 触发一次后自动禁用，杜绝并发重复派发
 *
 *   Worker 处理完后通过 zephyr_epoll_reactivate() 重新激活。
 *---------------------------------------------------------------------------*/
int zephyr_epoll_add(int epoll_fd, int fd, zephyr_event_t* ev)
{
    if (epoll_fd < 0 || fd < 0 || ev == NULL) {
        return -1;
    }

    /* 强制非阻塞 */
    if (__set_nonblocking(fd) < 0) {
        perror("[Zephyr Epoll] Failed to set O_NONBLOCK");
        return -2;
    }

    /* 把 epoll_fd 存进全家桶，方便 worker 线程后续 re-arm */
    ev->epoll_fd = epoll_fd;

    struct epoll_event ee;
    ee.events   = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
    ee.data.ptr = ev;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ee) < 0) {
        perror("[Zephyr Epoll] epoll_ctl ADD failed");
        return -3;
    }
    return 0;
}

/*---------------------------------------------------------------------------
 * 重新激活被 EPOLLONESHOT 禁用的事件
 *
 * Worker 线程在完成一次请求处理后调用本函数，让连接重新接收内核事件。
 * extra_events 可传入 EPOLLOUT 以便续发未完成的数据。
 *---------------------------------------------------------------------------*/
int zephyr_epoll_reactivate(int epoll_fd, int fd, zephyr_event_t* ev, uint32_t extra_events)
{
    if (epoll_fd < 0 || fd < 0 || ev == NULL) {
        return -1;
    }

    struct epoll_event ee;
    ee.events   = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT | extra_events;
    ee.data.ptr = ev;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ee) < 0) {
        perror("[Zephyr Epoll] epoll_ctl MOD (reactivate) failed");
        return -2;
    }
    return 0;
}

/*---------------------------------------------------------------------------
 * 从 epoll 红黑树摘除节点，并关闭 fd
 *---------------------------------------------------------------------------*/
int zephyr_epoll_del(int epoll_fd, int fd)
{
    if (epoll_fd < 0 || fd < 0) return -1;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        perror("[Zephyr Epoll] epoll_ctl DEL failed");
        return -2;
    }

    close(fd);
    return 0;
}

/*---------------------------------------------------------------------------
 * 阻塞等待事件就绪
 *---------------------------------------------------------------------------*/
int zephyr_epoll_wait(int epoll_fd, struct epoll_event* events, int max_events, int timeout)
{
    if (epoll_fd < 0 || events == NULL || max_events <= 0) return -1;

    int nready = epoll_wait(epoll_fd, events, max_events, timeout);

    if (nready < 0) {
        if (errno == EINTR) {
            return 0; /* 被信号中断，安全返回 0 */
        }
        perror("[Zephyr Epoll] epoll_wait failed");
        return -2;
    }

    return nready;
}
