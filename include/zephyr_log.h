#ifndef __ZEPHYR_LOG_H
#define __ZEPHYR_LOG_H

/* ================================================================
 * Zephyr 日志子系统（客户端库）
 *
 * 架构：客户端 → Unix Domain Socket (DGRAM) → zephyr_logd 守护进程
 *
 * 特性：
 *   - 非阻塞发送（守护进程不在或队列满时丢弃日志，不影响主业务）
 *   - 5 个日志级别（DEBUG / INFO / WARN / ERROR / FATAL）
 *   - 零动态分配（栈上构造消息）
 *   - 自动重连（守护进程重启后自动恢复）
 * ================================================================ */

/* 日志级别 */
typedef enum {
    ZLOG_DEBUG = 0,
    ZLOG_INFO  = 1,
    ZLOG_WARN  = 2,
    ZLOG_ERROR = 3,
    ZLOG_FATAL = 4,
} zlog_level_t;

/* ---- 公开 API ---- */

/**
 * @brief 初始化日志客户端（连接到守护进程）
 *
 * 应在服务器启动时调用一次。非阻塞——若守护进程尚未启动，
 * 日志消息将被静默丢弃，不会阻塞主业务。
 *
 * @param socket_path Unix Domain Socket 路径（如 "/tmp/zephyr_logd.sock"）
 * @return           0 成功，<0 失败
 */
int zlog_init(const char* socket_path);

/**
 * @brief  发送一条日志到守护进程
 *
 * @param level   日志级别
 * @param file    源文件名（通常传 __FILE__）
 * @param line    行号（通常传 __LINE__）
 * @param func    函数名（通常传 __func__）
 * @param fmt     printf 风格格式化字符串
 * @param ...     可变参数
 */
void zlog_write(zlog_level_t level,
                const char* file,
                int line,
                const char* func,
                const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 5, 6)))
#endif
;

/**
 * @brief 关闭日志客户端连接
 */
void zlog_close(void);

/* ---- 便捷宏（推荐使用） ---- */

#define ZLOG_DEBUG(fmt, ...)  zlog_write(ZLOG_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define ZLOG_INFO(fmt, ...)   zlog_write(ZLOG_INFO,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define ZLOG_WARN(fmt, ...)   zlog_write(ZLOG_WARN,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define ZLOG_ERROR(fmt, ...)  zlog_write(ZLOG_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define ZLOG_FATAL(fmt, ...)  zlog_write(ZLOG_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#endif
