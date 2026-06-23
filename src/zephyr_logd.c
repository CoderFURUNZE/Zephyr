/* ================================================================
 * Zephyr 日志守护进程 (zephyr_logd)
 *
 * 职责：
 *   1. 监听 Unix Domain Socket (DGRAM)，接收来自业务进程的日志消息
 *   2. 写入日志文件，支持按大小自动轮转
 *   3. 以守护进程方式运行（fork + setsid）
 *   4. 响应 SIGHUP 重新打开日志文件（配合 logrotate）
 *
 * 编译为独立二进制，不链入主服务器。
 *
 * 用法：
 *   ./bin/zephyr_logd [-s <socket_path>] [-f <log_file>] [-m <max_size_mb>]
 *
 * 默认值：
 *   socket : /tmp/zephyr_logd.sock
 *   log    : ./zephyr.log
 *   max    : 10 MB
 * ================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>

/* ---- 默认配置 ---- */
#define DEFAULT_SOCK_PATH  "/tmp/zephyr_logd.sock"
#define DEFAULT_LOG_FILE   "./zephyr.log"
#define DEFAULT_MAX_SIZE   (10 * 1024 * 1024)  /* 10 MB */
#define PID_FILE           "/tmp/zephyr_logd.pid"

/* ---- 全局状态 ---- */
static volatile int g_running   = 1;
static volatile int g_reopen    = 0;  /* SIGHUP 置 1 */
static const char*  g_log_path  = NULL;
static int64_t      g_max_size  = DEFAULT_MAX_SIZE;
static FILE*        g_log_fp    = NULL;
static int64_t      g_current_size = 0;

/* ================================================================
 * 信号处理
 * ================================================================ */

static void __sig_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGHUP) {
        g_reopen = 1;
    }
}

/* ================================================================
 * 写入 PID 文件
 * ================================================================ */

static void __write_pid(void)
{
    FILE* fp = fopen(PID_FILE, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    }
}

static void __remove_pid(void)
{
    unlink(PID_FILE);
}

/* ================================================================
 * 守护进程化
 * ================================================================ */

static int __daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("[Zephyr LogD] fork() failed");
        return -1;
    }
    if (pid > 0) {
        /* 父进程：退出 */
        _exit(0);
    }

    /* 子进程 */
    if (setsid() < 0) {
        perror("[Zephyr LogD] setsid() failed");
        return -1;
    }

    /* 第二次 fork，彻底脱离终端 */
    pid = fork();
    if (pid < 0) {
        perror("[Zephyr LogD] second fork() failed");
        return -1;
    }
    if (pid > 0) {
        _exit(0);
    }

    /* 重定向标准 fd 到 /dev/null */
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > 2) close(null_fd);
    }

    /* 设置文件创建掩码 */
    umask(022);

    /* 切换工作目录到 / (避免占用卸载点) */
    chdir("/");

    return 0;
}

/* ================================================================
 * 日志文件操作
 * ================================================================ */

/* ---- 递归创建父目录（类似 mkdir -p） ---- */
static int __mkdir_parents(const char* path)
{
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* 逐级创建 */
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);          /* 忽略已存在错误 */
            *p = '/';
        }
    }
    return 0;
}

static int __open_log_file(void)
{
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }

    /* 确保父目录存在 */
    __mkdir_parents(g_log_path);

    g_log_fp = fopen(g_log_path, "a");
    if (g_log_fp == NULL) {
        fprintf(stderr, "[Zephyr LogD] Cannot open log file: %s\n", g_log_path);
        return -1;
    }

    /* 禁用缓冲——每条日志立即落盘 */
    setvbuf(g_log_fp, NULL, _IONBF, 0);

    /* 获取当前文件大小 */
    fseek(g_log_fp, 0, SEEK_END);
    g_current_size = ftell(g_log_fp);

    return 0;
}

/* ================================================================
 * 日志轮转：重命名当前文件为 .1，打开新文件
 * ================================================================ */

static void __rotate_log(void)
{
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }

    char backup[1024];
    snprintf(backup, sizeof(backup), "%s.1", g_log_path);
    rename(g_log_path, backup);

    __open_log_file();

    /* 写一条轮转标记 */
    if (g_log_fp) {
        time_t now = time(NULL);
        char buf[64];
        ctime_r(&now, buf);
        /* 去掉换行 */
        buf[strcspn(buf, "\n")] = '\0';
        fprintf(g_log_fp, "--- Log rotated at %s ---\n", buf);
    }
}

/* ================================================================
 * 写入一条日志到文件
 * ================================================================ */

static void __write_to_file(const char* msg, int len)
{
    if (g_log_fp == NULL) return;

    /* 检查是否需要轮转 */
    if (g_current_size + len > g_max_size) {
        __rotate_log();
    }

    if (g_log_fp == NULL) return;

    fprintf(g_log_fp, "%s\n", msg);
    g_current_size += len + 1;
}

/* ================================================================
 * 创建并绑定 Unix Domain Socket
 * ================================================================ */

static int __create_socket(const char* path)
{
    /* 清理旧 socket 文件 */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("[Zephyr LogD] socket() failed");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[Zephyr LogD] bind() failed");
        close(fd);
        return -1;
    }

    /* 宽松权限，允许任意进程写入 */
    chmod(path, 0666);

    return fd;
}

/* ================================================================
 * 主循环：接收 DGRAM 并写入文件
 * ================================================================ */

static void __event_loop(int sock_fd)
{
    char buf[65536];

    while (g_running) {

        ssize_t n = recv(sock_fd, buf, sizeof(buf) - 1, 0);

        if (n < 0) {
            if (errno == EINTR) {
                /* 被信号打断，检查是否需要 reopen 或退出 */
                if (g_reopen) {
                    g_reopen = 0;
                    __open_log_file();
                }
                continue;
            }
            perror("[Zephyr LogD] recv() error");
            continue;
        }

        if (n == 0) continue; /* DGRAM 不应收到 0，防御 */

        buf[n] = '\0';
        __write_to_file(buf, (int)n);
    }
}

/* ================================================================
 * 打印用法
 * ================================================================ */

static void __usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [-s <socket_path>] [-f <log_file>] [-m <max_size_mb>] [-fg]\n"
        "  -s   Unix socket path       (default: %s)\n"
        "  -f   Log file path          (default: %s)\n"
        "  -m   Max log size in MB     (default: 10)\n"
        "  -fg  Run in foreground      (default: daemonize)\n",
        prog, DEFAULT_SOCK_PATH, DEFAULT_LOG_FILE);
}

/* ================================================================
 * 主入口
 * ================================================================ */

int main(int argc, char* argv[])
{
    const char* sock_path = DEFAULT_SOCK_PATH;
    const char* log_path  = DEFAULT_LOG_FILE;
    int foreground        = 0;

    /* ---- 解析命令行参数 ---- */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            int mb = atoi(argv[++i]);
            g_max_size = (int64_t)mb * 1024 * 1024;
            if (g_max_size <= 0) g_max_size = DEFAULT_MAX_SIZE;
        } else if (strcmp(argv[i], "-fg") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            __usage(argv[0]);
            return 0;
        }
    }

    g_log_path = log_path;

    /* ---- 守护进程化 ---- */
    if (!foreground) {
        if (__daemonize() < 0) {
            return 1;
        }
    }

    /* ---- 注册信号 ---- */
    signal(SIGINT,  __sig_handler);
    signal(SIGTERM, __sig_handler);
    signal(SIGHUP,  __sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* ---- 打开日志文件 ---- */
    if (__open_log_file() < 0) {
        fprintf(stderr, "[Zephyr LogD] Failed to open log file, exiting.\n");
        return 1;
    }

    /* ---- 创建监听 socket ---- */
    int sock_fd = __create_socket(sock_path);
    if (sock_fd < 0) {
        fprintf(stderr, "[Zephyr LogD] Failed to create socket, exiting.\n");
        return 1;
    }

    /* ---- 写 PID 文件 ---- */
    __write_pid();

    printf("[Zephyr LogD] Started (pid=%d)\n", getpid());
    printf("[Zephyr LogD] Socket : %s\n", sock_path);
    printf("[Zephyr LogD] Log    : %s (max %ld MB)\n",
           log_path, (long)(g_max_size / (1024 * 1024)));

    /* ---- 事件循环 ---- */
    __event_loop(sock_fd);

    /* ---- 清理 ---- */
    printf("[Zephyr LogD] Shutting down...\n");

    close(sock_fd);
    unlink(sock_path);
    if (g_log_fp) fclose(g_log_fp);
    __remove_pid();

    printf("[Zephyr LogD] Goodbye.\n");
    return 0;
}
