# Zephyr 高性能 HTTP 服务器 — 面试题库（完整版）

> 覆盖全部模块，按依赖关系编排，由浅入深。
> 每题附带参考答案，末尾归纳薪资对应策略。
> 总计 95 题。

---

## 目录

| 章节 | 模块 | 题数 |
|------|------|------|
| 一 | 环形队列 (queue) | 8 |
| 二 | 动态缓冲区 (buffer) | 10 |
| 三 | Epoll 封装 | 10 |
| 四 | 自适应线程池 | 12 |
| 五 | HTTP 解析器 (含零拷贝专题) | 15 |
| 六 | 主程序总装 (main.c) | 12 |
| 七 | 定时器 (timer) | 10 |
| 八 | 日志系统 (log) | 8 |
| 九 | 综合/系统设计 | 8 |
| 十 | 调试/性能/对比 | 6 |
| 十一 | 薪资策略建议 | — |

---

## 一、环形队列 (queue)

### 题 1：环形队列判空判满 —— 为什么 capacity 要 +1？

**问**：你的 `queue_init` 里写了 `capacity = capacity + 1`，为什么要多申请一个格子？

**答**：环形队列区分「空」和「满」有三种主流方案：

| 方案 | 空条件 | 满条件 | 代价 |
|------|--------|--------|------|
| 牺牲一个格子 | `front == rear` | `(rear+1) % cap == front` | 浪费一个槽 |
| 额外 count 变量 | `count == 0` | `count == cap` | 多一个变量 |
| flag 标记位 | `front==rear && flag==0` | `front==rear && flag==1` | 多一个 flag |

我选择方案一：空间开销最小，逻辑最简洁，且不需要额外的原子操作保护。

如果不 +1，初始化 `front=rear=0`，空和满的条件同时成立，无法区分。

---

### 题 2：入队和出队为什么「先移指针，再读写数据」？

**问**：你的 `queue_en` 先 `rear = (rear+1) % capacity` 再 `memcpy`，为什么不反过来？

**答**：因为初始状态 `front=rear=0`，位置 0 是隔离格。先移指针再写入，第一个元素写在位置 1 而非位置 0。

```
初始: [空][ ][ ][ ][ ]    入队A: [空][A][ ][ ][ ]
       ↑↑                          ↑   ↑
     front,rear                  front rear
```

出队同理：先移 front 再读数据。如果反过来先读写再移指针——元素会写在隔离格位置 0，判空判满公式全部失效。

---

### 题 3：为什么用 `void*` + `memcpy` 实现泛型？

**问**：你这个队列能存 task_t，也能存 int，怎么做到的？

**答**：C 语言没模板，我用 `void*` + `memcpy` + 调用者提供 `sizeof` 手工泛型。

```c
queue_init(&queue, 1000, sizeof(task_t));

// 入队
memcpy((char*)q->s + q->rear * q->size, data, q->size);
```

队列完全不知道类型，只按字节操作。

**追问**：对比 C++ 模板的劣势？ 类型不安全——sizeof 传错静默出错；每次入队都要 memcpy，而 C++ 模板可以内联优化。

---

### 题 4：出队后为什么要 `memset` 清零？

**答**：队列存储 `task_t` 含函数指针 `void (*job)(void*)`。不清零 → bug 导致野调用 → 执行随机地址 → 不可预测行为。清零 → 调用 NULL → 立刻 crash → Fail Fast。

---

### 题 5：这个队列是线程安全的吗？

**答**：不是。队列内部无锁。线程安全由调用方（线程池的 mutex）保证。分层设计——数据结构不关心并发，并发控制在上层。

**追问**：如果做成内置锁的线程安全版，`queue_is_full` + `queue_en` 之间有什么问题？ TOCTOU——判满和入队之间可能被插入，需要条件变量保护。

---

### 题 6：为什么底层数组用 `calloc` 而不是 `malloc`？

**答**：`calloc` 自动清零 + 内部检查乘法溢出。虽本项目容量小不会溢出，但好习惯值得保持。

---

### 题 7：`(rear+1) % capacity == front` 为什么能判断满？

**答**：rear 指向最后一个元素，它的下一步就是隔离格，隔离格由 front 守护。rear 的下一步追上了 front → 满了。

---

### 题 8：如果要支持动态扩容怎么改？

**答**：增加 `queue_resize` → realloc 底层数组，把绕回尾部的数据搬到正确位置。线程池侧满队时不阻塞等待，改为触发扩容。但生产环境一般不这么做——队列满是背压信号，应该限流而非无限堆队列。

---

## 二、动态缓冲区 (zephyr_buffer)

### 题 9：缓冲区的四指针模型

**问**：`data + capacity + read_idx + write_idx` 四个字段怎么管理读写？

**答**：天然划分三个区间：

```
[0, read_idx)           → 已消费垃圾
[read_idx, write_idx)   → 有效待读数据
[write_idx, capacity)   → 空闲可写空间
```

`readable = write_idx - read_idx`，`writable = capacity - write_idx`。

**追问**：为什么不用环形？ HTTP 解析器零拷贝——method/url 指针指向 buffer 内部，环形绕回会切断请求行。

---

### 题 10：碎片压缩为什么过半才搬？

**答**：权衡「搬移开销」与「空间回收」。每消费 1 字节搬一次 → O(n²) 不可接受。过半才搬——长连接下垃圾累积到一半，一次性 memmove 收回头部空间。阈值 capacity/2 是经典平衡点。

用 memmove 非 memcpy 因为源和目标可能重叠。

---

### 题 11：全部消费完为什么直接归零而不是 memmove？

**答**：两个 int 赋值 vs memmove——O(1) vs O(n)。更快。

---

### 题 12：`readv` 双通道读取的原理和价值

**答**：ET 模式正确性要求，非性能优化。buffer 只剩 200 但 socket 来 500：

```
普通 read → 读 200，剩 300 困在内核，ET 不再通知
readv: vec[0]=buffer剩余 + vec[1]=extrabuf 64KB → 一次吸干
溢出的在 extrabuf → realloc + memcpy 回 buffer
```

一次系统调用解决「不知道能读多少但必须全读完」的矛盾。

---

### 题 13：`readv` 的 64KB 限制

**答**：如果 socket 来 128KB，剩余数据困在内核。对 GET 请求实际不触发，但设计上有限制。修复：检查 n == writable + sizeof(extrabuf) → 两个 iovec 全满 → 再调一次 readv。

---

### 题 14：为什么 `iovcnt` 不固定为 2？

**答**：buffer 空闲 ≥ 64KB 时只用 1 个 iovec——减少内核 iovec 拷贝开销。微优化但每条连接都走的路径值得。

---

### 题 15：`zephyr_buf_write_fd` 四种返回值协议

**答**：

```
n > 0   → 发出 n 字节，可能还有剩余
n == 0  → 无数据可发
n == -2 → EAGAIN，内核发送缓冲区满，等 EPOLLOUT
n == -1 → 致命错误，关连接
```

把非阻塞 I/O 不确定性转化为上层清晰决策分支。

---

### 题 16：n==-2 和 n>0&&remaining!=0 处理一样，为什么还区分？

**答**：对连接命运一样，但对**日志和调试**有价值——n==-2=门口就卡了，remaining!=0=发到一半被卡，排查方向不同。

---

### 题 17：缓冲区攻击面 —— Slowloris

**答**：恶意客户端永远不发 `\r\n` → readv 不断追加 → realloc 反复扩容 → OOM。两道防线：timer 30s 超时 + buffer 硬上限 `MAX_HEADER_SIZE 65536`。限时 + 限大小。

---

### 题 18：realloc 频繁扩容怎么优化？

**答**：HTTP 请求生命周期 buffer 从 1024 出发，最多扩 2~3 次，相对状态机解析和磁盘 I/O 可忽略。真要优化：内存池预分配，或翻倍扩容。

---

## 三、Epoll 封装

### 题 19：epoll 三件套

| 调用 | 作用 |
|------|------|
| `epoll_create(size)` | 内核创建 epoll 实例（红黑树），size 被忽略 |
| `epoll_ctl(op, fd, &event)` | ADD/MOD/DEL 节点 |
| `epoll_wait(events, max, timeout)` | 阻塞等就绪，timeout: -1=永久, 0=立即, >0=N毫秒 |

---

### 题 20：select/poll/epoll 三者对比

| | select | poll | epoll |
|---|--------|------|-------|
| 监听方式 | 每次传 fd_set | 每次传 pollfd 数组 | 内核维护红黑树 |
| 时间复杂度 | O(n) | O(n) | O(1) 只遍历就绪链表 |
| 最大 fd 数 | 1024 | 无上限 | 无上限 |
| 适用 | 少量 fd | 少量 fd | 大量 fd 大部分空闲 |

HTTP 服务器数千连接只有几个活跃——epoll 是唯一正确选择。

---

### 题 21：ET vs LT

**答**：LT 只要数据在就反复通知；ET 只在「空→有数据」瞬间通知一次。ET 减少系统调用，但必须搭配非阻塞 I/O + 循环读直到 EAGAIN。

---

### 题 22：EPOLLONESHOT 为什么是并发安全关键？

**答**：没有 ONESHOT：主线程把 fd=5 投给线程 A → 下一轮 epoll_wait 又返回 fd=5 → 线程 B 也拿到 → 两个线程同时操作同一个 buffer → 数据乱序。

有 ONESHOT：触发一次自动禁用，只有线程 A 在处理，处理完 reactivate。内核级互斥锁，零开销。

---

### 题 23：为什么 listen_fd 不用 ONESHOT？

**答**：listen_fd 的 accept 在主循环，没有并发竞争。加了 ONESHOT 反而坏事——accept 一个后禁用，没人 reactivate，后续连接丢失。

---

### 题 24：EPOLLRDHUP vs EPOLLHUP

**答**：RDHUP=对端半关闭（发了 FIN），写端还好；HUP=对端完全断开。你的项目都走 cleanup，但 RDHUP 在 EPOLLOUT 续发场景仍可能单独出现——优雅的读完最后一个字节后对端关了。

---

### 题 25：EINTR 处理

**答**：Ctrl+C → SIGINT → epoll_wait 返回 -1 errno=EINTR。返回 0 而非报错，主循环继续检查 g_server_running → 优雅退出。

---

### 题 26：epoll_create 参数被忽略为什么还传 1024？

**答**：无害兼容习惯，与 MAX_EVENTS 保持一致。现代做法：`epoll_create1(EPOLL_CLOEXEC)`。

---

### 题 27：`epoll_data_t` union

**答**：`data.ptr` 挂结构体指针——事件触发时一次解引用拿到整个连接上下文，不需要 fd→ctx 哈希表。还可挂 `fd`/`u32`/`u64`。

---

### 题 28：>1024 并发怎么改？

**答**：MAX_EVENTS 改大（栈转堆分配）；线程上限可能上调；真正瓶颈不在就绪事件数——1024 同时就绪对应上万连接。

---

## 四、自适应线程池

### 题 29：整体架构

```
任务队列(环形,容量1000) → worker×N(4~32) cond_wait 等任务
                        → admin×1 每秒 sleep(1) 检查负载 → 扩/缩容
```

---

### 题 30：扩容缩容策略

```c
MAXJOB=32  MIN_FREE=4  MAX_FREE=8  STEP=4
扩容: busy==live && live<32 → +4
缩容: free>8 && live>4      → -4
```

步长 4 避免频繁振荡。

---

### 题 31：缩容「信号接力」

**答**：`cond_signal` 唤醒对象不确定——可能唤醒去干活而非自杀。admin 发 4 个信号期望缩 4 个，实际可能只缩 2 个。自杀线程退出前再 signal 一个接力——「我走了，再叫个人来填自杀名额」。

无接力最坏：`exit_threads` 永远消费不完，线程池缩不下来。

---

### 题 32：条件变量虚假唤醒

**答**：POSIX 允许 spurious wakeup。用 `while (empty && !shutdown)` 而非 `if`——醒来重新检查条件，防止空任务执行。

---

### 题 33：admin 为什么 sleep(1)？

**答**：tradeoff——极简实现 vs 实时性。最长 1 秒后才扩容，对请求波动不敏感的场景可接受。更高响应性可以 timedwait 替代。

---

### 题 34：pool_destroy 销毁顺序

**答**：shutdown=1 → broadcast → join(admin) 先 → join(each worker) 后 → free。先 join admin 确保它不再碰 workers 数组。

---

### 题 35：两锁分工

**答**：mut_pool 保护队列+free+live；mut_busy 只保护 busy 计数。admin 读 busy 时 worker 可同时取任务——减少锁竞争。

---

### 题 36：task_t 函数指针

**答**：策略模式。线程池不关心任务内容——`(mytask.job)(mytask.arg)`。新增任务类型无需改线程池。

---

### 题 37：队列满了池 add_task 会阻塞主线程

**答**：是的——这是背压机制。但主线程阻塞 → 新连接无人 accept。改进：非阻塞 `try_add`，失败回 503。

---

### 题 38：初始 4 个线程浪费吗

**答**：用少量内存换冷启动延迟。4×8MB=32MB 虚拟内存，可忽略。前 4 个请求无需等 pthread_create。

---

### 题 39：worker 崩溃怎么恢复

**答**：当前没处理——worker segfault → 整个进程死。生产级：pthread_cleanup_push/pop + 看门狗进程监控重启。

---

### 题 40：MAXJOB=32 和 CPU 核数关系

**答**：理想值 = 核数 × (1+I/O等待比)。8 核 32 线程合理，2 核则过多。自适应：`max_threads = sysconf(_SC_NPROCESSORS_ONLN) * 4`。

---

## 五、HTTP 解析器

### 题 41：状态机

**答**：PARSE_REQUESTLINE → PARSE_HEADERS → PARSE_PARSE_DONE。天然支持断点续传——LINE_OPEN 保留状态，下次数据到了继续。

---

### 题 42：`__get_line` 的零拷贝实现（基础问）

**问**：你如何在不对数据做 strcpy 的情况下切出一行 HTTP 请求？

**答**：原地把 `\r\n` 改成 `\0\0`，返回行首指针作为 C 字符串。method/url/version 三个字段直接指向 `input_buffer` 内部。

```
修改前: [G][E][T][\r][\n]  →  修改后: [G][E][T][\0][\0]
                                 ↑
                              buf_start → "GET"
```

流程：`for(p=buf_start; p<buf_end; p++)` 找 `\r\n` → `*p='\0'; *(p+1)='\0'` → `*line_start=buf_start` → `read_idx+=跳过`。

全程无 `strdup`/`strcpy`/`malloc`，**一次内存都不分**。

---

### 题 42b：应用层零拷贝 vs 内核层零拷贝（深度追问）

**问**：你刚才说的零拷贝是哪个层面的？和 `sendfile()` 有什么不同？

**答**：分层讲——

**我的零拷贝是应用层**：解析阶段不把 buffer 数据拷到独立字符串，指针直接指向 buffer 内部。省掉一次用户态内存拷贝。

**`sendfile()` 是内核层**：文件页缓存直接 DMA 到网卡，不经过用户态，连 read/write 系统调用都省了。

```
应用层零拷贝（你的项目）：
  内核缓冲区 → readv → 用户态buffer → 状态机指针直接指向buffer
                             ↑
                      拷了一次（必须的）

内核层零拷贝（sendfile）：
  文件页缓存 → DMA → 网卡
  用户态完全不过手
```

**追问**：那你的项目能不能用 `sendfile`？ 能——文件响应时文件内容走 `sendfile(fd, file_fd, ...)` 替代 `read→write`。但响应头（状态行+Content-Type）还是得自己拼，所以做不到 100% 零拷贝。

---

### 题 42c：零拷贝的风险（高阶追问）

**问**：method/url 指针直接指向 buffer 内部，buffer 一旦被清或被覆盖，这些指针就悬空了。你怎么保证安全？

**答**：确实有这个风险。我的防线是——

1. **生命周期绑定**：`req` 是栈变量，只在 `http_business_handler` 内有效。buffer 在这个函数执行期间不会被修改（读数据是主线程干的，ONESHOT 保证主线程不会在这个 fd 上再读数据）。

2. **用完即弃**：`req.method`/`req.url` 只在解析和 `make_response` 期间使用，函数返回前这些指针就不再被引用了。

3. **悬空是不存在的**：因为 buffer 的 `retrieve`（碎片压缩）只在 `write_fd` 之后触发——而 `write_fd` 在 `make_response` 之后执行。解析和使用期间，buffer 区域绝对稳定。

**追问**：如果我要把 `req` 存到队列以后再用呢？那零拷贝就不安全了——buffer 可能被下一个请求覆盖。这时必须 `strdup` 把数据拷出来。这就是 Nginx 的做法：用内存池管理生命周期，需要跨阶段使用的数据拷到连接池或请求池。

---

### 题 43：半包处理 —— LINE_OPEN

**答**：数据不完整时 read_idx 不动，reactivate 等更多数据。下次数据追加,状态机从断点继续。

---

### 题 44：只解析 Connection 头

**答**：单站点静态文件服务器——Host/UA/Accept 不影响行为。只解析影响连接行为的 Connection。刻意简化。

---

### 题 45：HTTP/1.0 vs 1.1 keep-alive

**答**：默认 1.1 持久连接，检测到 HTTP/1.0 则关闭。符合 RFC 7230 §6.3。HTTP/1.0 也可通过 `Connection: keep-alive` 显式覆盖。

---

### 题 46：make_response 完整流程

**答**：方法校验(405) → URL 安全 + ".." 检测(400) → stat 文件(404) → MIME 查表 → 状态行 → 响应头+空行 → 读文件 → 失败回退(500)。

---

### 题 47：500 为什么归零 write_idx

**答**：半路翻车——200 状态行已写入 buffer，文件读一半失败。不归零 → 客户端收到混着 200+500 的乱响应。归零 = 揉掉信纸重写。

---

### 题 48：MIME 为什么线性查找

**答**：15 个条目，最多对比 15 次 strcasecmp。哈希表初始化/哈希计算的开销 > 查找本身。小数据量下简单算法优于复杂结构。

---

### 题 49：读文件为什么循环 read

**答**：POSIX read 不保证一次返回全部字节——短读可能因信号中断等。防御式编程：不假设 I/O 行为。

---

### 题 50：URL 超长怎么办

**答**：buffer 自动扩容，但恶意极长 URL 可 OOM。应加 URL 长度上限检查。

---

### 题 51：HTTP Pipelining 支持吗

**答**：不支持。两个请求粘连在 buffer，第一个解析完第二个没人处理。改进：发完响应检查 buffer 是否还有可读数据。

---

### 题 52：__build_error_body 为什么也检查空间

**答**：函数自治——不依赖调用方的空间准备。各自负责自己的扩容检查。

---

## 六、主程序总装 (main.c)

### 题 53：完整数据流

**答**：TCP → accept → 挂 epoll(ONESHOT) → 设 30s 超时 → EPOLLIN → readv 吸数据 → 投线程池 → 解析→组装→write → reactivate 或 cleanup。

---

### 题 54：事件优先级

**答**：Error > EPOLLOUT > EPOLLIN。先判死刑，再清积压，最后读新数据。

---

### 题 55：主线程只读不提

**答**：Reactor 分离——主线程 I/O 永不阻塞，worker CPU 可慢。accept 延迟极低，慢请求不挡快请求，多核并行。

---

### 题 56：accept 也 while

**答**：ET 模式同时 3 个连接 → 通知一次 → 不循环则丢两个。

---

### 题 57：安全退出

**答**：SIGINT → g_server_running=0 → DEL listen_fd → close epoll → pool_destroy（等 worker 干完）→ timer_destroy。池最后销毁——worker 可能还在处理。

---

### 题 58：epoll_wait timeout=-1

**答**：无事件就睡，不空转。EINTR 返回 0 → 主循环继续。

---

### 题 59：新连接资源分配

**答**：event+input_buf+output_buf+epoll节点+timer≈2.5KB/连接+内核开销。1024连接≈2.5MB。

---

### 题 60：handle_epollout 为什么独立

**答**：复用(http_business_handler 和续发共用发送逻辑）;逻辑分离（续发不占 worker）。

---

### 题 61：g_server_running 为什么 volatile

**答**：禁止编译器缓存。信号处理函数异步修改，volatile 确保主循环每次从内存读。

**追问**：volatile 是原子吗？ 不是。对多线程应 atomic_int；对信号，volatile sig_atomic_t 是标准保证安全的类型。

---

### 题 62：SO_REUSEADDR 不加怎样

**答**：重启时 TIME_WAIT(60s) 端口不可重用，bind 失败。

---

### 题 63：worker 里直接 cleanup 安全吗

**答**：安全——ONESHOT 保证只有这个 worker 碰 fd。但定时器回调不能直接 cleanup——和 worker 并发。所以定时器只 shutdown+flag。

---

### 题 64：主线程阻塞投递怎么优化

**答**：无阻塞 try_add → 失败回 503，或用无界队列（不推荐）。更好：ring buffer + 主线程检查。

---

## 七、定时器 (timer)

### 题 65：为什么最小堆

| 方案 | 插入 | 删除 | 取最小 |
|------|------|------|--------|
| 最小堆 | O(log n) | O(log n) | O(1) |
| 时间轮 | O(1) | O(1) | O(1) |
| 红黑树 | O(log n) | O(log n) | O(log n) |

256 定时器 O(log 256)≈8 次比较，堆胜出。

---

### 题 66：堆索引从 1 开始

**答**：简化父子计算——parent=i/2 而非 (i-1)/2。浪费 heap[0] 可接受。

---

### 题 67：精确唤醒

**答**：pthread_cond_timedwait——堆顶 delta 毫秒后自动唤醒，或新定时器插入时 cond_signal 提前唤醒。无忙等。

---

### 题 68：纳秒进位

**答**：`tv_nsec >= 10^9` 时进位置秒——毫秒转纳秒可能溢出秒边界。

---

### 题 69：CLOCK_MONOTONIC vs REALTIME

**答**：REALTIME 可能被用户/NTP 调整 → 定时器全乱。MONOTONIC 单调递增不受影响。

---

### 题 70：ID 分配和回绕

**答**：id_seed 自增，MASK 到 31 位正数，0 表示「无定时器」。回绕不依赖唯一性——取消时按 id 遍历查找。

---

### 题 71：cancel 双向堆修复

**答**：取消非堆顶 → 堆尾换到该位置 → sift_up + sift_down 同时做，只有一个生效，恢复最小堆性质。

---

### 题 72：线程安全三条防线

**答**：① timed_out 标志 → worker 入口检查 return ② shutdown → EPOLLRDHUP → 主循环 cleanup ③ cleanup 前 cancel 定时器。定时器线程不释放内存。

---

### 题 73：shutdown vs close

**答**：close 直接释放 fd，但 worker 可能在用。shutdown 关读写但不释放 fd，且触发 epoll 事件。主循环到 cleanup 时 close 才真释放。

---

### 题 74：定时器在 worker 处理中触发

**答**：两种情况都安全——shutdown 后 worker 的 write 会失败，走自己的错误路径 → cleanup 取消定时器。shutdown 不释放内存，worker 可安全处理失败。

---

## 八、日志系统 (log)

### 题 75：为什么 Unix Domain Socket + 守护进程

**答**：异步隔离——磁盘阻塞在守护进程侧，主业务不阻塞。daemon 崩则 send 静默丢弃，不影响业务。

---

### 题 76：SOCK_DGRAM vs SOCK_STREAM

**答**：DGRAM 无连接、非阻塞、sendto 即走。可能丢包但对日志可接受。STREAM 连接管理成本不值得。

---

### 题 77：MSG_DONTWAIT | MSG_NOSIGNAL

**答**：非阻塞 + 不触发 SIGPIPE。日志发送无论如何不影响业务。

---

### 题 78：__attribute__((format(printf,5,6)))

**答**：GCC 扩展——编译器按 printf 规则检查参数，编译时发现类型不匹配。

---

### 题 79：双 fork daemonize

**答**：fork→setsid→fork→dup2 /dev/null→umask→chdir /。标准 POSIX 守护进程化。第二次 fork 确保不是会话组长，无法重新获得终端。

---

### 题 80：日志轮转

**答**：自动按大小(10MB) + SIGHUP 手动配合 logrotate。双保险。

---

### 题 81：PID 文件

**答**：外部工具(logrotate/监控脚本)需要知道 daemon PID 发信号。退出时 unlink。

---

### 题 82：5 个日志级别

**答**：DEBUG<INFO<WARN<ERROR<FATAL——log4j/syslog/nginx 的事实标准。

---

## 九、综合/系统设计

### 题 83：支持 POST 怎么改

**答**：解析器加 PARSE_BODY + Content-Length 上限 + 业务层根据 path 路由。I/O 框架不变。

---

### 题 84：支持 HTTPS 怎么改

**答**：accept 后 TLS 握手 → SSL_read/write 替代 readv/write。Reactor+线程池架构不变。

---

### 题 85：设计模式

**答**：Reactor、Half-Sync/Half-Async、状态机、策略模式(task_t.job)、观察者(epoll)、生产者-消费者(主线程→队列→worker)、单例(g_timer_mgr)。

---

### 题 86：支持 WebSocket

**答**：识别 Upgrade: websocket → 切换帧协议处理 → 新增 websocket_handler。底层 I/O 复用。

---

### 题 87：怎么 bench

**答**：wrk -t4 -c100 -d30s。关键指标：Req/sec、Latency p99、Transfer/sec。对比 nginx 静态文件模式。

---

### 题 88：怎么分析瓶颈

**答**：perf top(CPU 热点)、strace -c(系统调用统计)、valgrind callgrind(调用图)。逐步加压看吞吐量曲线平台即瓶颈。

---

### 题 89：相比 nginx 差在哪

**答**：单 Reactor vs 多进程、无内存池、无缓存、无反代、无 SSL、无配置文件、无限流。恰恰是面试好材料——「我知道差在哪，差的都是下一步」。

---

### 题 90：从头重写改什么

**答**：内存池、one-loop-per-thread、配置文件、buffer 上限、POST+CGI、单元测试。

---

## 十、调试/性能/对比

### 题 91：gdb 调试死锁

**答**：`gdb -p pid` → `info threads` → `thread apply all bt`。卡在 lock 的线程 → 找锁持有者 → 回溯。

---

### 题 92：valgrind 查内存泄漏

**答**：`valgrind --leak-check=full --track-origins=yes ./bin/zephyr`。关注 definitely lost 和 indirectly lost。

---

### 题 93：strace 分析瓶颈

**答**：`strace -c -p pid` → 统计系统调用次数/错误率。EAGAIN 比例高 → ET 空转；futex 多 → 锁竞争。

---

### 题 94：怎么压测

**答**：wrk 递增并发 10→50→100→500→1000 看吞吐量曲线。加 keep-alive header 模拟真实场景。

---

### 题 95：Zephyr vs Nginx 代码级对比

| 维度 | Nginx | Zephyr |
|------|-------|--------|
| 事件 | 多进程 epoll | 单进程 epoll+池 |
| 内存 | 内存池 | malloc/free |
| 请求 | 完整 RFC 7230 | 简化 GET 状态机 |
| 模块 | 动态加载 | 单体 |
| 代码量 | ~150K | ~2.8K |

**话术**：「Nginx 工业化，Zephyr 教育性。但 2800 行我可以讲清楚每一行为什么在那个位置。」

---

## 十一、薪资策略建议

### 面试角色定位

| 方向 | 匹配度 |
|------|--------|
| 后端/C++/Go 服务端 | ⭐⭐⭐⭐⭐ |
| 基础架构/中间件 | ⭐⭐⭐⭐ |
| 高性能网络编程 | ⭐⭐⭐⭐⭐ |
| 系统软件 | ⭐⭐⭐⭐ |

### 15 分钟讲授策略

```
0-2分: 一句话概述 + 架构图
2-5分: 三个亮点(ONESHOT+池/状态机断点/扩缩容)——画图讲
5-12分: 攻坚一问(Slowloris/readv限制/定时器三条防线)——展示深度
12-15分: 主动暴露改进方向——显格局
```

### 15K 标准

| 层次 | 表现 | 薪资 |
|------|------|------|
| 基础 | 说清是什么 | 8-12K |
| 进阶 | 说清为什么 + tradeoff | 12-18K |
| 高阶 | 主动谈攻击面/边界/改进 | 18-25K |

答对 70% + 逻辑清晰展开 → **15K 稳**。

---

> 生成日期: 2026-06-23
> 总题目数: 95 题（基础 42 + 深度追问 53）
> 覆盖: 8 个模块 + 综合设计 + 调试/性能/对比 + 薪资策略
