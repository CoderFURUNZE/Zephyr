#ifndef __ZEPHYR_TIMER_H
#define __ZEPHYR_TIMER_H

#include <stdint.h>

/* ================================================================
 * Zephyr 定时器子系统（独立模块，零外部依赖）
 *
 * 实现：最小堆 + 专用定时线程 + 条件变量精确唤醒
 * 特性：
 *   - 一次性定时器 & 周期性定时器
 *   - O(log n) 插入 / 删除，O(1) 获取最近到期定时器
 *   - 线程安全（互斥锁保护）
 *   - 回调在定时器线程内执行，使用者需自行处理线程安全
 * ================================================================ */

/* 回调函数类型 */
typedef void (*zephyr_timer_cb)(void* arg);

/* 不透明句柄 */
typedef struct zephyr_timer_mgr zephyr_timer_mgr_t;

/* ================================================================
 * 公开 API
 * ================================================================ */

/**
 * @brief 创建定时器管理器并启动内部线程
 * @return 成功返回管理器指针，失败返回 NULL
 */
zephyr_timer_mgr_t* zephyr_timer_create(void);

/**
 * @brief 添加一个定时器
 *
 * @param tm         定时器管理器指针
 * @param delay_ms   首次触发延迟（毫秒），0=立即触发
 * @param interval_ms 周期间隔（毫秒），0=一次性定时器
 * @param cb         回调函数
 * @param arg        回调参数
 * @return           >=0 定时器 ID（用于取消），<0 失败
 */
int zephyr_timer_add(zephyr_timer_mgr_t* tm,
                     int64_t delay_ms,
                     int64_t interval_ms,
                     zephyr_timer_cb cb,
                     void* arg);

/**
 * @brief 取消指定定时器（已触发的不会被取消）
 * @param tm       定时器管理器指针
 * @param timer_id 由 zephyr_timer_add 返回的 ID
 * @return         0 成功，<0 失败（如 ID 不存在）
 */
int zephyr_timer_cancel(zephyr_timer_mgr_t* tm, int timer_id);

/**
 * @brief 获取当前活跃定时器数量
 */
int zephyr_timer_count(zephyr_timer_mgr_t* tm);

/**
 * @brief 销毁定时器管理器，释放全部资源
 */
void zephyr_timer_destroy(zephyr_timer_mgr_t* tm);

#endif
