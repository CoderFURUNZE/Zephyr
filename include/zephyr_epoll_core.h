#ifndef __ZEPHYR_EPOLL_CORE_H
#define __ZEPHYR_EPOLL_CORE_H

#include <sys/epoll.h>

/* 前向声明，避免循环依赖 */
struct zephyr_buffer;

/**
 * @struct zephyr_event_t
 * @brief 跨模块公共事件上下文（解耦纽带）
 *
 * 每个客户端连接持有一个该结构体实例，挂在 epoll 红黑树节点的 data.ptr 上。
 * 当事件触发时，主循环通过 ptr 瞬间拿到该连接的所有上下文信息。
 */
typedef struct zephyr_event {
    int fd;                              /* 客户端 Socket 文件描述符 */
    int epoll_fd;                        /* 所属 epoll 句柄（worker 线程 re-arm 用） */
    struct zephyr_buffer* input_buffer;  /* 该连接专属的输入缓冲区 */
    struct zephyr_buffer* output_buffer; /* 该连接专属的输出缓冲区 */

    /* 通用业务回调函数指针（void* 与线程池 task_t.job 签名一致） */
    void (*handler_callback)(void* ev);

    /* 连接状态标志 */
    int keep_alive;                      /* HTTP keep-alive 标记 */
    int pending_output;                  /* 还有未发完的数据，需要 EPOLLOUT */
    int timer_id;                        /* 关联的定时器 ID（0=无），用于取消 */
    volatile int timed_out;              /* 1=已超时，定时器线程写入 */

    void* arg;                           /* 预留自定义上下文 */
} zephyr_event_t;

/* ================================================================
 * Epoll 核心 API
 * ================================================================ */

/** 创建 epoll 实例 */
int zephyr_epoll_create(void);

/**
 * 向 epoll 红黑树添加/修改套接字的监听事件
 * 强制采用 ET + EPOLLONESHOT 模式，杜绝并发重复派发
 *
 * @param epoll_fd  epoll 句柄
 * @param fd        要监听的套接字
 * @param ev        绑定的公共事件上下文指针（挂载至 epoll_data_t.ptr）
 * @return          成功返回 0，失败返回负数
 */
int zephyr_epoll_add(int epoll_fd, int fd, zephyr_event_t* ev);

/**
 * 重新激活一个被 EPOLLONESHOT 禁用的事件
 * Worker 线程处理完请求后调用此函数，让连接重新接收事件
 *
 * @param epoll_fd     epoll 句柄
 * @param fd           要重新激活的套接字
 * @param ev           事件上下文（用于重新挂载 ptr）
 * @param extra_events 额外的事件标志（如 EPOLLOUT 用于续发未完成的数据）
 * @return             成功返回 0，失败返回负数
 */
int zephyr_epoll_reactivate(int epoll_fd, int fd, zephyr_event_t* ev, uint32_t extra_events);

/** 从 epoll 红黑树删除节点并关闭 fd */
int zephyr_epoll_del(int epoll_fd, int fd);

/** 阻塞等待网络事件（对 epoll_wait 的轻量封装） */
int zephyr_epoll_wait(int epoll_fd, struct epoll_event* events, int max_events, int timeout);

#endif
