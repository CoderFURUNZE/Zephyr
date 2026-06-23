#ifndef __POOL_H
#define __POOL_H

#include <pthread.h>
#include "queue.h"

#define MAXJOB 32//最多有32个线程
#define MIN_FREE_NR 4//最少有4个空闲线程
#define MAX_FREE_NR 8//最多有8个空闲线程
#define STEP 4//增量

//定义线程池的结构
typedef struct
{
    pthread_t *workers;//工作线程的起始地址
    pthread_t admin_tid;//管理者线程标识
    queue_t *task_queue;//任务队列
    
    //线程池结构
    int max_threads;//最多的线程个数
    int min_free_threads;//最少的线程线程个数
    int max_free_threads;//最多的空闲线程个数
    int busy_threads;//目前执行任务的线程个数
    int free_threads;//目前空闲线程的个数
    int live_threads;//活着的线程数
    int exit_threads;//要终止的线程个数
    int shutdown;//标记关闭线程池(1代表关闭)

    pthread_mutex_t mut_pool;//整个线程池的锁
    pthread_mutex_t mut_busy;//busy单独加一个互斥量
    pthread_cond_t queue_not_empty;//当队列不为空的时候通知
    pthread_cond_t queue_not_full;//当队列不为满的时候通知
}pool_t;


//任务结构
typedef struct
{
    void (*job)(void *s);
    void *arg;
}task_t;

//函数的接口
extern int pool_init(pool_t **mypool,int capacity);
extern int pool_add_task(pool_t *mypool,const task_t *t);
extern void pool_destroy(pool_t *mypool);

#endif

