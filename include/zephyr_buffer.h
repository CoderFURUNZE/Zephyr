#ifndef __ZEPHYR_BUFFER_H
#define __ZEPHYR_BUFFER_H
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
/**
 * @brief Zephyr 动态流式缓冲区结构体
 */
typedef struct zephyr_buffer {
    char* data;          // 指向实际内存块的指针（通过 malloc/realloc 动态分配）
    int capacity;        // 当前缓冲区的总容量（总大小）
    int read_idx;        // 读指针下标：从这里开始读取数据给状态机
    int write_idx;       // 写指针下标：从这里开始把网络数据写入 Buffer
} zephyr_buffer_t;

/**
 * @brief 创建并初始化一个动态缓冲区
 * @param init_size 初始内存大小（例如默认给 1024 字节）
 * @return 成功返回缓冲区指针，失败返回 NULL
 */
zephyr_buffer_t* zephyr_buf_create(int init_size);

/**
 * @brief 销毁缓冲区，释放内存
 */
void zephyr_buf_destroy(zephyr_buffer_t* buf);

/**
 * @brief 获取缓冲区中当前可读（未解析）的字节数
 */
ssize_t zephyr_buf_readable_bytes(const zephyr_buffer_t* buf);

/**
 * @brief 获取缓冲区中当前空闲可写的字节数
 */
ssize_t zephyr_buf_writable_bytes(const zephyr_buffer_t* buf);

/**
 * @brief 核心 I/O 接口：直接从网络 Socket FD 循环读取数据到 Buffer 中（自动处理 ET 模式和扩容）
 * @param buf 缓冲区指针
 * @param fd 客户端的 Socket 文件描述符
 * @param saved_errno 传出参数，用于记录非阻塞 read 产生的真实系统错误（如 EAGAIN）
 * @return 返回实际读到的字节数，返回 0 代表客户端断开连接，负数代表出错
 */
ssize_t zephyr_buf_read_fd(zephyr_buffer_t* buf, int fd, int* saved_errno);

/**
 * @brief 核心 I/O 接口：将 Buffer 中组装好的响应数据，通过 Socket FD 发送给客户端
 * @param buf 缓冲区指针
 * @param fd 客户端的 Socket 文件描述符
 * @return  > 0  实际发送的字节数（可能小于可读字节数，即部分发送）
 *          = 0  没有待发送数据
 *          = -2 EAGAIN/EWOULDBLOCK，内核发送缓冲区满，需等 EPOLLOUT 后续传
 *          = -1 致命错误（对端断开等）
 */
ssize_t zephyr_buf_write_fd(zephyr_buffer_t* buf, int fd);

/**
 * @brief 当状态机消费了数据后，向前移动读下标
 * @note  当 read_idx 推进后，会自动触发碎片压缩（memmove），
 *        将未消费数据搬移到缓冲区头部，防止长期连接内存无限增长。
 */
void zephyr_buf_retrieve(zephyr_buffer_t* buf, size_t len);

#endif

