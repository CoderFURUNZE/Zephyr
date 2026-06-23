# Zephyr High-Performance Web Server
# Makefile

CC       := gcc
CFLAGS   := -Wall -Wextra -std=gnu11 -I.
LDFLAGS  := -lpthread

TARGET        := bin/zephyr
LOGDAEMON     := bin/zephyr_logd

SRCDIR        := src

# 主服务器源码
SERVER_SRCS   := $(SRCDIR)/queue.c              \
                 $(SRCDIR)/zephyr_buffer.c      \
                 $(SRCDIR)/zephyr_threadpool.c  \
                 $(SRCDIR)/zephyr_http_parser.c \
                 $(SRCDIR)/zephyr_epoll_core.c  \
                 $(SRCDIR)/zephyr_timer.c       \
                 $(SRCDIR)/zephyr_log.c

# 日志守护进程源码（独立二进制）
LOGD_SRCS     := $(SRCDIR)/zephyr_logd.c

.PHONY: all clean run logd debug

all: $(TARGET) $(LOGDAEMON)

# Debug 构建：无优化 + 调试符号，GDB 友好
debug: CFLAGS += -g -O0 -DDEBUG
debug: all

$(TARGET): main.c $(SERVER_SRCS)
	@mkdir -p bin
	$(CC) $(CFLAGS) main.c $(SERVER_SRCS) -o $@ $(LDFLAGS)
	@echo "[OK] Build successful → $(TARGET)"

$(LOGDAEMON): $(LOGD_SRCS)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LOGD_SRCS) -o $@ $(LDFLAGS)
	@echo "[OK] Build successful → $(LOGDAEMON)"

run: $(TARGET)
	./$(TARGET)

# 启动日志守护进程（后台运行）
logd: $(LOGDAEMON)
	./$(LOGDAEMON) -fg &
	@echo "[OK] Log daemon started"

# 停止日志守护进程
logd-stop:
	@if [ -f /tmp/zephyr_logd.pid ]; then \
		kill $$(cat /tmp/zephyr_logd.pid) 2>/dev/null && echo "[OK] Log daemon stopped" || echo "[!] Log daemon not running"; \
	else \
		echo "[!] PID file not found"; \
	fi

clean:
	rm -rf bin/zephyr bin/zephyr_logd
	rm -f /tmp/zephyr_logd.sock /tmp/zephyr_logd.pid
	@echo "[OK] Clean complete"
