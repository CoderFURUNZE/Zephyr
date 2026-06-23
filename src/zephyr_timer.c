#include "../include/zephyr_timer.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>

/* ================================================================
 * 内部常量
 * ================================================================ */

#define TIMER_MAX       256         /* 最大并发定时器数 */
#define TIMER_ID_MASK   0x7FFFFFFF  /* ID 回绕掩码 */

/* ================================================================
 * 单个定时器节点
 * ================================================================ */

typedef struct {
    int       id;            /* 唯一标识（用于外部取消） */
    int       active;        /* 1=活跃, 0=已取消 / 空闲槽位 */
    int64_t   expiry_ms;     /* 绝对到期时间（单调毫秒） */
    int64_t   interval_ms;   /* 周期间隔，0=一次性 */
    zephyr_timer_cb cb;      /* 回调 */
    void*     arg;           /* 回调参数 */
} timer_node_t;

/* ================================================================
 * 定时器管理器
 * ================================================================ */

struct zephyr_timer_mgr {
    timer_node_t  heap[TIMER_MAX];  /* 最小堆数组（索引从 1 开始） */
    int           size;             /* 当前堆大小 */

    int           id_seed;          /* 自增 ID 种子 */
    int           running;          /* 运行标志 */

    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       thread;
};

/* ================================================================
 * 辅助：获取单调毫秒时间戳
 * ================================================================ */

static int64_t __now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ================================================================
 * 最小堆操作
 *
 * 堆索引从 1 开始（简化父子计算）：
 *   parent(i) = i / 2
 *   left(i)   = i * 2
 *   right(i)  = i * 2 + 1
 * ================================================================ */

static void __heap_swap(timer_node_t* a, timer_node_t* b)
{
    timer_node_t tmp = *a;
    *a = *b;
    *b = tmp;
}

/* 上滤 */
static void __heap_sift_up(timer_node_t* heap, int idx)
{
    while (idx > 1) {
        int parent = idx / 2;
        if (heap[idx].expiry_ms < heap[parent].expiry_ms) {
            __heap_swap(&heap[idx], &heap[parent]);
            idx = parent;
        } else {
            break;
        }
    }
}

/* 下滤 */
static void __heap_sift_down(timer_node_t* heap, int size, int idx)
{
    while (1) {
        int smallest = idx;
        int left  = idx * 2;
        int right = idx * 2 + 1;

        if (left <= size && heap[left].expiry_ms < heap[smallest].expiry_ms) {
            smallest = left;
        }
        if (right <= size && heap[right].expiry_ms < heap[smallest].expiry_ms) {
            smallest = right;
        }
        if (smallest != idx) {
            __heap_swap(&heap[idx], &heap[smallest]);
            idx = smallest;
        } else {
            break;
        }
    }
}

/* 插入节点 */
static void __heap_push(timer_node_t* heap, int* size, timer_node_t node)
{
    (*size)++;
    heap[*size] = node;
    __heap_sift_up(heap, *size);
}

/* 弹出堆顶 */
static timer_node_t __heap_pop(timer_node_t* heap, int* size)
{
    timer_node_t top = heap[1];
    heap[1] = heap[*size];
    (*size)--;
    if (*size > 0) {
        __heap_sift_down(heap, *size, 1);
    }
    return top;
}

/* ================================================================
 * 定时器线程主循环
 * ================================================================ */

static void* __timer_thread(void* arg)
{
    zephyr_timer_mgr_t* tm = (zephyr_timer_mgr_t*)arg;

    pthread_mutex_lock(&tm->mutex);

    while (tm->running) {

        /* ---- 堆为空 → 无限等待直到有新定时器加入 ---- */
        if (tm->size == 0) {
            pthread_cond_wait(&tm->cond, &tm->mutex);
            continue;
        }

        /* ---- 查看堆顶（最近到期） ---- */
        timer_node_t* top = &tm->heap[1];
        int64_t now   = __now_ms();
        int64_t delta = top->expiry_ms - now;

        if (delta <= 0) {
            /* 到期：弹出、解锁执行回调、再锁回来 */
            timer_node_t fired = __heap_pop(tm->heap, &tm->size);

            pthread_mutex_unlock(&tm->mutex);

            /* 在锁外执行回调，避免死锁 */
            if (fired.cb) {
                fired.cb(fired.arg);
            }

            pthread_mutex_lock(&tm->mutex);

            /* 周期性定时器：重新入堆 */
            if (fired.interval_ms > 0 && fired.active) {
                fired.expiry_ms = __now_ms() + fired.interval_ms;
                __heap_push(tm->heap, &tm->size, fired);
                /* 唤醒可能正在等待的 add 操作 */
                pthread_cond_signal(&tm->cond);
            }
        } else {
            /* 还没到，等待 delta 毫秒或被新定时器插入打断 */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);

            ts.tv_sec  += (time_t)(delta / 1000);
            ts.tv_nsec += (long)((delta % 1000) * 1000000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec  += 1;
                ts.tv_nsec -= 1000000000L;
            }

            /* pthread_cond_timedwait 会在超时或 cond_signal 时返回 */
            pthread_cond_timedwait(&tm->cond, &tm->mutex, &ts);
        }
    }

    pthread_mutex_unlock(&tm->mutex);
    return NULL;
}

/* ================================================================
 * 公开 API
 * ================================================================ */

zephyr_timer_mgr_t* zephyr_timer_create(void)
{
    zephyr_timer_mgr_t* tm = malloc(sizeof(zephyr_timer_mgr_t));
    if (tm == NULL) return NULL;
    memset(tm, 0, sizeof(zephyr_timer_mgr_t));

    tm->size    = 0;
    tm->id_seed = 1;
    tm->running = 1;

    pthread_mutex_init(&tm->mutex, NULL);
    pthread_cond_init(&tm->cond, NULL);

    if (pthread_create(&tm->thread, NULL, __timer_thread, tm) != 0) {
        pthread_mutex_destroy(&tm->mutex);
        pthread_cond_destroy(&tm->cond);
        free(tm);
        return NULL;
    }

    return tm;
}

int zephyr_timer_add(zephyr_timer_mgr_t* tm,
                     int64_t delay_ms,
                     int64_t interval_ms,
                     zephyr_timer_cb cb,
                     void* arg)
{
    if (tm == NULL || cb == NULL) return -1;

    pthread_mutex_lock(&tm->mutex);

    if (tm->size >= TIMER_MAX) {
        pthread_mutex_unlock(&tm->mutex);
        fprintf(stderr, "[Zephyr Timer] Timer pool exhausted (max=%d)\n", TIMER_MAX);
        return -2;
    }

    /* 分配 ID（跳过 0 和负数） */
    int id = tm->id_seed & TIMER_ID_MASK;
    if (id <= 0) id = 1;
    tm->id_seed = id + 1;

    timer_node_t node;
    node.id          = id;
    node.active      = 1;
    node.expiry_ms   = __now_ms() + delay_ms;
    node.interval_ms = interval_ms;
    node.cb          = cb;
    node.arg         = arg;

    __heap_push(tm->heap, &tm->size, node);

    /* 唤醒定时器线程（可能新定时器比堆顶更早到期） */
    pthread_cond_signal(&tm->cond);

    pthread_mutex_unlock(&tm->mutex);
    return id;
}

int zephyr_timer_cancel(zephyr_timer_mgr_t* tm, int timer_id)
{
    if (tm == NULL || timer_id <= 0) return -1;

    pthread_mutex_lock(&tm->mutex);

    for (int i = 1; i <= tm->size; i++) {
        if (tm->heap[i].id == timer_id && tm->heap[i].active) {
            tm->heap[i].active = 0;

            /* 与堆尾交换后弹出，然后修复堆 */
            if (i != tm->size) {
                __heap_swap(&tm->heap[i], &tm->heap[tm->size]);
            }
            tm->size--;

            if (i <= tm->size) {
                /* 从 i 位置双向修复 */
                __heap_sift_up(tm->heap, i);
                __heap_sift_down(tm->heap, tm->size, i);
            }

            pthread_mutex_unlock(&tm->mutex);
            return 0;
        }
    }

    pthread_mutex_unlock(&tm->mutex);
    return -2; /* ID 未找到 */
}

int zephyr_timer_count(zephyr_timer_mgr_t* tm)
{
    if (tm == NULL) return 0;
    pthread_mutex_lock(&tm->mutex);
    int cnt = tm->size;
    pthread_mutex_unlock(&tm->mutex);
    return cnt;
}

void zephyr_timer_destroy(zephyr_timer_mgr_t* tm)
{
    if (tm == NULL) return;

    pthread_mutex_lock(&tm->mutex);
    tm->running = 0;
    pthread_cond_signal(&tm->cond);
    pthread_mutex_unlock(&tm->mutex);

    pthread_join(tm->thread, NULL);

    pthread_mutex_destroy(&tm->mutex);
    pthread_cond_destroy(&tm->cond);
    free(tm);
}
