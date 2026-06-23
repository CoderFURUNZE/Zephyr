#include "../include/zephyr_threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

// 辅助函数：寻找空闲的线程槽位（找值为 0 的槽位）
static int __get_threads_pos(pthread_t *jobs, int n)
{
	int i = 0;//循环变量

	for(i = 0; i < n; i++)//轮询查找工作线程结构中是否存在没有使用过的位置
		if(jobs[i] == (pthread_t)-1)
			return i;
	for(i = 0; i < n; i++)
	{//检查线程是否存在
		if(pthread_kill(jobs[i], 0) == ESRCH)//判断当前位置的工作线程是否不存在
			return i;
	}
	return -1;
}

// 工作线程后台循环
static void *thread_worker(void *arg)
{
    pool_t *me = (pool_t *)arg;
    task_t mytask;
    while (1)
    {
        pthread_mutex_lock(&me->mut_pool);
        
        // 1. 队列为空且没打烊，就一直等
        while (queue_is_empty(me->task_queue) && !me->shutdown)
        {
            pthread_cond_wait(&me->queue_not_empty, &me->mut_pool);
            
            // 2. 醒来后优先检查是否需要因“缩容”而自杀
            if (me->exit_threads > 0 && !me->shutdown)
            {
                me->exit_threads--;
                me->live_threads--;
                me->free_threads--;
                pthread_cond_signal(&me->queue_not_empty); // 🟢 修复：接力通知，防止信号丢失导致死锁
                pthread_mutex_unlock(&me->mut_pool);
                pthread_exit(0);
            }
        }

        // 3. 如果线程池已经处于关闭状态，立刻退出
        if (me->shutdown)
        {
            pthread_mutex_unlock(&me->mut_pool);
            pthread_exit(0);
        }

        // 4. 真正拿到任务
        queue_de(me->task_queue, &mytask);
        pthread_cond_signal(&me->queue_not_full); // 唤醒可能阻塞在添加任务处的线程
        
        // 5. 开始干活，更新状态
        me->free_threads--;
        pthread_mutex_unlock(&me->mut_pool);

        pthread_mutex_lock(&me->mut_busy);
        me->busy_threads++;
        pthread_mutex_unlock(&me->mut_busy);

        // 🟢 执行真正的业务逻辑（如具体的 HTTP 解析或数据发送）
        (mytask.job)(mytask.arg);

        pthread_mutex_lock(&me->mut_busy);
        me->busy_threads--;
        pthread_mutex_unlock(&me->mut_busy);

        pthread_mutex_lock(&me->mut_pool);
        me->free_threads++;
        pthread_mutex_unlock(&me->mut_pool);
    }
    return NULL;
}

// 管理者线程：负责监控和动态伸缩
static void *thread_admin(void *arg)
{
    pool_t *me = (pool_t *)arg;
    while (1)
    {
        sleep(1); // 每秒监控一次

        pthread_mutex_lock(&me->mut_pool);
        if (me->shutdown)
        {
            pthread_mutex_unlock(&me->mut_pool);
            break;
        }

        pthread_mutex_lock(&me->mut_busy);
        int busy_cnt = me->busy_threads;
        pthread_mutex_unlock(&me->mut_busy);

        int live_cnt = me->live_threads;
        int free_cnt = me->free_threads;

        // 🟢 扩容策略：忙碌线程满载，且未达到最大线程上限
        if (busy_cnt == live_cnt && live_cnt < me->max_threads)
        {
            int add_cnt = 0;
            for (int i = 0; i < STEP && me->live_threads < me->max_threads; i++)
            {
                int pos = __get_threads_pos(me->workers, me->max_threads);
                if (pos != -1)
                {
                    if (pthread_create(&(me->workers[pos]), NULL, thread_worker, me) == 0)
                    {
                        me->live_threads++;
                        me->free_threads++;
                        add_cnt++;
                    }
                }
            }
        }

        // 🟢 缩容策略：空闲线程太多，且活着的线程多于初始保底线
        if (free_cnt > me->max_free_threads && live_cnt > MIN_FREE_NR)
        {
            me->exit_threads = STEP;
            for (int i = 0; i < STEP; i++)
            {
                pthread_cond_signal(&me->queue_not_empty); // 唤醒闲着的线程让其自杀
            }
        }
        pthread_mutex_unlock(&me->mut_pool);
    }
    pthread_exit(0);
}

// 线程池初始化
int pool_init(pool_t **mypool, int capacity)
{
    if (mypool == NULL || capacity <= 0) return -1;

    pool_t *me = malloc(sizeof(pool_t));
    if (me == NULL) return -1;
    memset(me, 0, sizeof(pool_t));

    me->max_threads = MAXJOB; // 🟢 修复：线程上限由头文件宏控制
    me->min_free_threads = MIN_FREE_NR;
    me->max_free_threads = MAX_FREE_NR;
    me->busy_threads = 0;
    me->live_threads = 0;
    me->exit_threads = 0;
    me->shutdown = 0;

    // 分配工作线程数组空间，全部初始化为 -1（空槽位标记）
    me->workers = malloc(me->max_threads * sizeof(pthread_t));
    if (me->workers == NULL) {
        free(me);
        return -2;
    }
    for (int i = 0; i < me->max_threads; i++) {
        me->workers[i] = (pthread_t)-1;
    }

    // 初始化你的自定义任务队列（容量为传入的 capacity）
    queue_init(&me->task_queue, capacity, sizeof(task_t));

    pthread_mutex_init(&me->mut_pool, NULL);
    pthread_mutex_init(&me->mut_busy, NULL);
    pthread_cond_init(&me->queue_not_empty, NULL);
    pthread_cond_init(&me->queue_not_full, NULL);

    pthread_mutex_lock(&me->mut_pool);
    // 创建保底初始数量的工作线程
    for (int i = 0; i < MIN_FREE_NR; i++)
    {
        if (pthread_create(&(me->workers[i]), NULL, thread_worker, me) == 0) {
            me->live_threads++;
            me->free_threads++;
        }
    }
    // 启动管理者线程
    pthread_create(&me->admin_tid, NULL, thread_admin, me);
    pthread_mutex_unlock(&me->mut_pool);

    *mypool = me;
    return 0;
}

// 投递任务
int pool_add_task(pool_t *mypool, const task_t *t)
{
    if (mypool == NULL || t == NULL) return -1;

    pthread_mutex_lock(&mypool->mut_pool);
    while (queue_is_full(mypool->task_queue) && !mypool->shutdown)
    {
        pthread_cond_wait(&mypool->queue_not_full, &mypool->mut_pool);
    }
    
    if (mypool->shutdown) {
        pthread_mutex_unlock(&mypool->mut_pool);
        return -1;
    }

    queue_en(mypool->task_queue, t);
    pthread_cond_signal(&mypool->queue_not_empty); // 唤醒一个线程去干活
    pthread_mutex_unlock(&mypool->mut_pool);
    return 0;
}

// 安全销毁线程池
void pool_destroy(pool_t *mypool)
{
    if (mypool == NULL) return;

    pthread_mutex_lock(&mypool->mut_pool);
    mypool->shutdown = 1;
    pthread_mutex_unlock(&mypool->mut_pool);

    // 1. 广播唤醒所有挂起的线程，它们醒来后会直接触发 shutdown 退出
    pthread_cond_broadcast(&mypool->queue_not_empty);
    pthread_cond_broadcast(&mypool->queue_not_full);

    // 2. 🟢 修复：极其安全的死等回收，不留一丝残渣
    pthread_join(mypool->admin_tid, NULL);
    for (int i = 0; i < mypool->max_threads; i++) {
        if (mypool->workers[i] != (pthread_t)-1) {
            pthread_join(mypool->workers[i], NULL);
        }
    }

    // 3. 彻底释放物理内存
    free(mypool->workers);
    queue_destroy(mypool->task_queue);
    pthread_mutex_destroy(&mypool->mut_pool);
    pthread_mutex_destroy(&mypool->mut_busy);
    pthread_cond_destroy(&mypool->queue_not_empty);
    pthread_cond_destroy(&mypool->queue_not_full);
    free(mypool);
    printf("[Zephyr Pool] Threadpool successfully destroyed.\n");
}

