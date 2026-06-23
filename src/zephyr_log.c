#include "../include/zephyr_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <pthread.h>

/* ================================================================
 * 内部常量
 * ================================================================ */

#define LOG_MSG_MAX   4096       /* 单条日志最大字节数 */
#define SOCK_PATH_MAX 108        /* Unix socket 路径最大长度 (sun_path) */

/* ================================================================
 * 全局状态（模块级单例）
 * ================================================================ */

static int  g_log_fd    = -1;        /* 连往守护进程的 socket */
static char g_sock_path[SOCK_PATH_MAX];
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 级别名称映射 */
static const char* __level_name(zlog_level_t level)
{
    switch (level) {
        case ZLOG_DEBUG: return "DEBUG";
        case ZLOG_INFO:  return "INFO ";
        case ZLOG_WARN:  return "WARN ";
        case ZLOG_ERROR: return "ERROR";
        case ZLOG_FATAL: return "FATAL";
        default:         return "?????";
    }
}

/* ================================================================
 * 内部：尝试连接守护进程
 * ================================================================ */

static int __connect_daemon(void)
{
    if (g_sock_path[0] == '\0') return -1;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    /* 设为非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ================================================================
 * 内部：格式化并发送日志消息
 *
 * 消息格式（纯文本，方便 grep / tail）：
 *   [2025-06-22 10:30:45.123] [ERROR] [main.c:42] [main] message...
 * ================================================================ */

static void __send_log(zlog_level_t level,
                       const char* file,
                       int line,
                       const char* func,
                       const char* formatted_msg)
{
    /* 拼接时间戳 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_buf;
    time_t sec = ts.tv_sec;
    localtime_r(&sec, &tm_buf);

    char packet[LOG_MSG_MAX];
    int len = snprintf(packet, sizeof(packet),
        "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] [%s] [%s:%d] [%s] %s",
        tm_buf.tm_year + 1900,
        tm_buf.tm_mon + 1,
        tm_buf.tm_mday,
        tm_buf.tm_hour,
        tm_buf.tm_min,
        tm_buf.tm_sec,
        ts.tv_nsec / 1000000,
        __level_name(level),
        file, line, func,
        formatted_msg);

    if (len < 0 || len >= (int)sizeof(packet)) {
        len = (int)sizeof(packet) - 1;
    }

    /* 非阻塞发送——socket 满或守护进程不在则静默丢弃 */
    ssize_t sent = send(g_log_fd, packet, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL);

    if (sent < 0) {
        /* 连接可能已断开，尝试重连后再次发送 */
        if (errno == ENOTCONN || errno == ECONNREFUSED || errno == EPIPE) {
            close(g_log_fd);
            g_log_fd = __connect_daemon();
            if (g_log_fd >= 0) {
                /* 重连成功，再试一次 */
                send(g_log_fd, packet, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL);
            }
        }
        /* 其他错误静默忽略——日志系统不能影响业务 */
    }
}

/* ================================================================
 * 公开 API
 * ================================================================ */

int zlog_init(const char* socket_path)
{
    if (socket_path == NULL) return -1;

    pthread_mutex_lock(&g_log_mutex);

    /* 如果已经初始化过，先关闭旧连接 */
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }

    strncpy(g_sock_path, socket_path, sizeof(g_sock_path) - 1);
    g_sock_path[sizeof(g_sock_path) - 1] = '\0';

    g_log_fd = __connect_daemon();

    pthread_mutex_unlock(&g_log_mutex);
    return 0; /* 永远返回成功——无守护进程只是静默丢弃 */
}

void zlog_write(zlog_level_t level,
                const char* file,
                int line,
                const char* func,
                const char* fmt, ...)
{
    if (g_log_fd < 0) return; /* 未初始化或无连接，静默丢弃 */

    /* 格式化用户消息 */
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_log_mutex);
    __send_log(level, file, line, func, msg);
    pthread_mutex_unlock(&g_log_mutex);
}

void zlog_close(void)
{
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
    g_sock_path[0] = '\0';
    pthread_mutex_unlock(&g_log_mutex);
}
