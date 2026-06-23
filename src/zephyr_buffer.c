#include "../include/zephyr_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <errno.h>

/* ===================================================================
 * 创建 / 销毁
 * =================================================================== */

zephyr_buffer_t* zephyr_buf_create(int init_size)
{
    if (init_size <= 0) return NULL;

    zephyr_buffer_t* me = malloc(sizeof(zephyr_buffer_t));
    if (me == NULL) return NULL;
    memset(me, 0, sizeof(zephyr_buffer_t));

    me->capacity  = init_size;
    me->read_idx  = 0;
    me->write_idx = 0;
    me->data      = malloc(sizeof(char) * me->capacity);

    if (me->data == NULL) {
        free(me);
        return NULL;
    }
    memset(me->data, 0, me->capacity);
    return me;
}

void zephyr_buf_destroy(zephyr_buffer_t* buf)
{
    if (buf == NULL) return;
    free(buf->data);
    free(buf);
}

/* ===================================================================
 * 容量查询
 * =================================================================== */

ssize_t zephyr_buf_readable_bytes(const zephyr_buffer_t* buf)
{
    if (buf == NULL) return 0;
    return buf->write_idx - buf->read_idx;
}

ssize_t zephyr_buf_writable_bytes(const zephyr_buffer_t* buf)
{
    if (buf == NULL) return 0;
    return buf->capacity - buf->write_idx;
}

/* ===================================================================
 * 读指针推进 + 碎片压缩
 *
 * 关键优化：消费数据后，若 read_idx > 0 且还有剩余数据，
 * 用 memmove 将未读数据搬移到缓冲区头部，重置读写指针。
 * 这样空闲空间始终在 write_idx 之后，杜绝碎片累积。
 * =================================================================== */

void zephyr_buf_retrieve(zephyr_buffer_t* buf, size_t len)
{
    if (buf == NULL || len <= 0) return;

    size_t readable = (size_t)zephyr_buf_readable_bytes(buf);

    if (len >= readable) {
        /* 全部消费完毕，直接归零（最高效） */
        buf->read_idx  = 0;
        buf->write_idx = 0;
    } else {
        buf->read_idx += (int)len;

        /*
         * 碎片压缩：当读指针跑过半程，把剩余数据搬回起点。
         * 阈值设在 capacity 的一半，平衡搬移开销与空间回收。
         */
        if (buf->read_idx > buf->capacity / 2) {
            size_t remaining = (size_t)(buf->write_idx - buf->read_idx);
            if (remaining > 0) {
                memmove(buf->data, buf->data + buf->read_idx, remaining);
            }
            buf->read_idx  = 0;
            buf->write_idx = (int)remaining;
        }
    }
}

/* ===================================================================
 * 核心 I/O：readv 双通道读取
 * =================================================================== */

ssize_t zephyr_buf_read_fd(zephyr_buffer_t* buf, int fd, int* save_errno)
{
    if (buf == NULL || buf->data == NULL) {
        if (save_errno) *save_errno = EINVAL;
        return -1;
    }

    /* 栈上 64KB 安全垫，确保 readv 一次吸干网络数据 */
    char extrabuf[65536];

    ssize_t writable = zephyr_buf_writable_bytes(buf);

    struct iovec vec[2];
    vec[0].iov_base = buf->data + buf->write_idx;
    vec[0].iov_len  = (size_t)(writable > 0 ? writable : 0);
    vec[1].iov_base = extrabuf;
    vec[1].iov_len  = sizeof(extrabuf);

    int iovcnt = (writable < (ssize_t)sizeof(extrabuf)) ? 2 : 1;

    ssize_t n = readv(fd, vec, iovcnt);

    if (n < 0) {
        if (save_errno) *save_errno = errno;
        return -1;
    } else if (n == 0) {
        return 0; /* 对端关闭 */
    } else if (n <= writable) {
        /* 数据全在 vec[0]，没有溢出 */
        buf->write_idx += (int)n;
    } else {
        /* 溢出：vec[0] 填满，尾巴掉进 extrabuf，精准扩容 */
        buf->write_idx  = buf->capacity;
        size_t extra_len = (size_t)(n - writable);

        int new_capacity = buf->capacity + (int)extra_len;
        char* new_data = realloc(buf->data, (size_t)new_capacity);
        if (new_data == NULL) {
            if (save_errno) *save_errno = ENOMEM;
            return -1;
        }

        buf->data     = new_data;
        buf->capacity = new_capacity;

        memcpy(buf->data + buf->write_idx, extrabuf, extra_len);
        buf->write_idx += (int)extra_len;
    }

    return n;
}

/* ===================================================================
 * 核心 I/O：write 发送
 *
 * 返回值约定（重要！）：
 *   > 0  实际发送的字节数（可能 < readable，即部分发送）
 *   = 0  无可发送数据
 *   = -2 EAGAIN，需等 EPOLLOUT 后续传
 *   = -1 致命错误
 * =================================================================== */

ssize_t zephyr_buf_write_fd(zephyr_buffer_t* buf, int fd)
{
    if (buf == NULL || buf->data == NULL) {
        return -1;
    }

    int readable = (int)zephyr_buf_readable_bytes(buf);
    if (readable <= 0) {
        return 0;
    }

    ssize_t n = write(fd, buf->data + buf->read_idx, (size_t)readable);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2; /* 非阻塞正常信号：内核发送缓冲区满 */
        }
        return -1;     /* 恶性错误（RST / 对端断开） */
    }

    /* 推进读指针（会自动触发碎片压缩） */
    zephyr_buf_retrieve(buf, (size_t)n);

    return n;
}
