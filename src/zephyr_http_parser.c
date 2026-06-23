#include "../include/zephyr_http_parser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>

/* ===================================================================
 * 内部辅助函数声明
 * =================================================================== */

static zephyr_line_state_t __get_line(zephyr_buffer_t* input_buf,
                                      char** line_start);

static const char* __get_mime_type(const char* path);

static int __read_file_to_buf(const char* filepath,
                              zephyr_buffer_t* output_buf);

static void __build_status_line(zephyr_buffer_t* buf,
                                int status_code,
                                int keep_alive);

static void __build_error_body(zephyr_buffer_t* buf, int status_code);

/* ===================================================================
 * MIME 类型映射表
 * =================================================================== */

typedef struct {
    const char* ext;
    const char* mime;
} mime_entry_t;

static const mime_entry_t MIME_TABLE[] = {
    { ".html",    "text/html; charset=utf-8"           },
    { ".htm",     "text/html; charset=utf-8"           },
    { ".css",     "text/css; charset=utf-8"            },
    { ".js",      "application/javascript"             },
    { ".json",    "application/json"                   },
    { ".png",     "image/png"                          },
    { ".jpg",     "image/jpeg"                         },
    { ".jpeg",    "image/jpeg"                         },
    { ".gif",     "image/gif"                          },
    { ".svg",     "image/svg+xml"                      },
    { ".ico",     "image/x-icon"                       },
    { ".txt",     "text/plain; charset=utf-8"          },
    { ".xml",     "application/xml"                    },
    { ".pdf",     "application/pdf"                    },
    { ".woff",    "font/woff"                          },
    { ".woff2",   "font/woff2"                         },
    { NULL,       "application/octet-stream"           },
};

static const char* __get_mime_type(const char* path)
{
    if (path == NULL) return "application/octet-stream";

    /* 找到最后一个 '.' */
    const char* dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";

    for (int i = 0; MIME_TABLE[i].ext != NULL; i++) {
        if (strcasecmp(dot, MIME_TABLE[i].ext) == 0) {
            return MIME_TABLE[i].mime;
        }
    }
    return "application/octet-stream";
}

/* ===================================================================
 * 初始化 / 重置
 * =================================================================== */

void zephyr_http_init_request(zephyr_http_request_t* req)
{
    if (req == NULL) return;
    req->method     = NULL;
    req->url        = NULL;
    req->version    = NULL;
    req->keep_alive = 1; /* HTTP/1.1 默认持久连接 (RFC 7230 §6.3) */
    req->state      = PARSE_REQUESTLINE;
}

/* ===================================================================
 * 内部辅助：从 Buffer 中切出一行（零拷贝）
 * =================================================================== */

static zephyr_line_state_t __get_line(zephyr_buffer_t* input_buf,
                                      char** line_start)
{
    char* buf_start = input_buf->data + input_buf->read_idx;
    char* buf_end   = input_buf->data + input_buf->write_idx;

    for (char* p = buf_start; p < buf_end; ++p) {
        if (*p == '\r') {
            if (p + 1 == buf_end) {
                return LINE_OPEN;          /* \r 是最后一个字符，\n 还没到 */
            }
            if (*(p + 1) == '\n') {
                *p          = '\0';
                *(p + 1)    = '\0';
                *line_start = buf_start;
                input_buf->read_idx += (int)(p - buf_start) + 2;
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (*p == '\n') {
            /* 兼容仅有 \n 的客户端 */
            *p          = '\0';
            *line_start = buf_start;
            input_buf->read_idx += (int)(p - buf_start) + 1;
            return LINE_OK;
        }
    }
    return LINE_OPEN; /* 半包，没找到行结束符 */
}

/* ===================================================================
 * 核心解析：有限状态机驱动
 * =================================================================== */

zephyr_http_state_t zephyr_http_parse_request(zephyr_http_request_t* req,
                                              zephyr_buffer_t* input_buf)
{
    if (req == NULL || input_buf == NULL) return PARSE_PARSE_DONE;

    char* line = NULL;
    zephyr_line_state_t line_status;

    while ((line_status = __get_line(input_buf, &line)) == LINE_OK) {

        switch (req->state) {

        case PARSE_REQUESTLINE: {
            /* 样例: "GET /index.html HTTP/1.1" */
            char* space1 = strchr(line, ' ');
            if (!space1) { req->state = PARSE_PARSE_DONE; return req->state; }
            *space1    = '\0';
            req->method = line;

            char* url_start = space1 + 1;
            char* space2 = strchr(url_start, ' ');
            if (!space2) { req->state = PARSE_PARSE_DONE; return req->state; }
            *space2     = '\0';
            req->url    = url_start;
            req->version = space2 + 1;

            /*
             * RFC 7230 §6.3: HTTP/1.1 默认持久连接，HTTP/1.0 默认关闭。
             * init 时已经设为 1，这里对 HTTP/1.0 做修正。
             */
            if (req->version && strcasecmp(req->version, "HTTP/1.0") == 0) {
                req->keep_alive = 0;
            }

            req->state = PARSE_HEADERS;
            break;
        }

        case PARSE_HEADERS: {
            /* 空行 = 请求头结束 */
            if (*line == '\0') {
                req->state = PARSE_PARSE_DONE;
                return req->state;
            }

            /* Connection 头：决定长短连接 */
            if (strncasecmp(line, "Connection:", 11) == 0) {
                char* value = line + 11;
                while (*value == ' ' || *value == '\t') value++;

                if (strncasecmp(value, "keep-alive", 10) == 0) {
                    req->keep_alive = 1;
                } else if (strncasecmp(value, "close", 5) == 0) {
                    req->keep_alive = 0;
                }
            }
            break;
        }

        case PARSE_BODY:
            /* 简化实现：GET 请求没有 body，直接完成 */
            req->state = PARSE_PARSE_DONE;
            return req->state;

        default:
            break;
        }
    }

    return req->state; /* LINE_OPEN → 等待更多数据 */
}

/* ===================================================================
 * 内部辅助：拼装状态行 + 通用响应头
 * =================================================================== */

static void __build_status_line(zephyr_buffer_t* buf,
                                int status_code,
                                int keep_alive)
{
    const char* status_text;
    switch (status_code) {
        case 200: status_text = "OK";                  break;
        case 400: status_text = "Bad Request";         break;
        case 404: status_text = "Not Found";           break;
        case 405: status_text = "Method Not Allowed";  break;
        case 500: status_text = "Internal Server Error"; break;
        default:  status_text = "Unknown";             break;
    }

    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: Zephyr/1.0\r\n"
        "Connection: %s\r\n",
        status_code, status_text,
        keep_alive ? "keep-alive" : "close");

    /* 确保响应头空间足够 */
    int writable = buf->capacity - buf->write_idx;
    if (writable < len) {
        int new_cap = buf->capacity + len + 256;
        char* new_data = realloc(buf->data, (size_t)new_cap);
        if (!new_data) return;
        buf->data     = new_data;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->write_idx, header, (size_t)len);
    buf->write_idx += len;
}

/* ===================================================================
 * 内部辅助：拼装错误页面 HTML
 * =================================================================== */

static void __build_error_body(zephyr_buffer_t* buf, int status_code)
{
    const char* title = status_code == 404 ? "Not Found"
                      : status_code == 405 ? "Method Not Allowed"
                      : status_code == 400 ? "Bad Request"
                      : "Internal Server Error";

    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html>\n"
        "<html><head><meta charset=\"utf-8\">"
        "<title>%d %s</title></head>\n"
        "<body><h1>%d %s</h1><hr><em>Zephyr/1.0</em></body></html>\n",
        status_code, title, status_code, title);

    /* Content-Length 头 */
    char cl_header[64];
    int cl_len = snprintf(cl_header, sizeof(cl_header),
        "Content-Length: %d\r\n", body_len);

    int writable = buf->capacity - buf->write_idx;
    int total = cl_len + body_len + 16; /* +16 for \r\n + safety */

    if (writable < total) {
        int new_cap = buf->capacity + total;
        char* new_data = realloc(buf->data, (size_t)new_cap);
        if (!new_data) return;
        buf->data     = new_data;
        buf->capacity = new_cap;
    }

    /* Content-Type + Content-Length + 空行 */
    int hdr_len = snprintf(buf->data + buf->write_idx, (size_t)(buf->capacity - buf->write_idx),
        "Content-Type: text/html; charset=utf-8\r\n%s\r\n",
        cl_header);
    buf->write_idx += hdr_len;

    /* 正文 */
    memcpy(buf->data + buf->write_idx, body, (size_t)body_len);
    buf->write_idx += body_len;
}

/* ===================================================================
 * 内部辅助：读取文件内容追加到 output_buf
 * 返回文件大小，失败返回 -1
 * =================================================================== */

static int __read_file_to_buf(const char* filepath,
                              zephyr_buffer_t* output_buf)
{
    struct stat st;
    if (stat(filepath, &st) < 0)   return -1;
    if (!S_ISREG(st.st_mode))      return -1; /* 不是普通文件 */

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return -1;

    off_t file_size = st.st_size;

    /* 确保 output_buf 有足够空间 */
    int writable = output_buf->capacity - output_buf->write_idx;
    if (writable < (int)file_size) {
        int new_cap = output_buf->capacity + (int)file_size + 256;
        char* new_data = realloc(output_buf->data, (size_t)new_cap);
        if (!new_data) { close(fd); return -1; }
        output_buf->data     = new_data;
        output_buf->capacity = new_cap;
    }

    /* 循环读取（防御短读） */
    ssize_t total = 0;
    while (total < file_size) {
        ssize_t n = read(fd,
                         output_buf->data + output_buf->write_idx + total,
                         (size_t)(file_size - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    output_buf->write_idx += (int)total;
    return (int)total;
}

/* ===================================================================
 * 业务响应接口：完整的 HTTP 响应组装
 * =================================================================== */

int zephyr_http_make_response(zephyr_http_request_t* req,
                              zephyr_buffer_t* output_buf)
{
    if (req == NULL || output_buf == NULL) return -1;

    /* ---- 1. 方法校验：仅支持 GET ---- */
    if (req->method == NULL || strcasecmp(req->method, "GET") != 0) {
        __build_status_line(output_buf, 405, req->keep_alive);
        __build_error_body(output_buf, 405);
        return 0;
    }

    /* ---- 2. URL 安全检查 ---- */
    char* path = req->url;
    if (path == NULL || strcmp(path, "/") == 0) {
        path = "/index.html";
    }

    /* 防止目录遍历攻击 */
    if (strstr(path, "..") != NULL) {
        __build_status_line(output_buf, 400, req->keep_alive);
        __build_error_body(output_buf, 400);
        return 0;
    }

    /* ---- 3. 构建文件系统路径 ---- */
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), DOCUMENT_ROOT "%s", path);

    /* ---- 4. 尝试读取文件 ---- */
    struct stat st;
    if (stat(filepath, &st) < 0 || !S_ISREG(st.st_mode)) {
        /* 404 Not Found */
        __build_status_line(output_buf, 404, req->keep_alive);
        __build_error_body(output_buf, 404);
        return 0;
    }

    /* ---- 5. 拼装 200 OK 响应 ---- */

    const char* mime = __get_mime_type(filepath);
    off_t file_size  = st.st_size;

    /* 写入状态行 */
    __build_status_line(output_buf, 200, req->keep_alive);

    /* 写入 Content-Type + Content-Length + 空行 */
    char remain_header[512];
    int remain_len = snprintf(remain_header, sizeof(remain_header),
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "\r\n",
        mime, (long)file_size);

    int writable = output_buf->capacity - output_buf->write_idx;
    if (writable < remain_len) {
        int new_cap = output_buf->capacity + remain_len + 256;
        char* new_data = realloc(output_buf->data, (size_t)new_cap);
        if (!new_data) return -2;
        output_buf->data     = new_data;
        output_buf->capacity = new_cap;
    }

    memcpy(output_buf->data + output_buf->write_idx, remain_header, (size_t)remain_len);
    output_buf->write_idx += remain_len;

    /* ---- 6. 读取并追加文件内容 ---- */
    int body_len = __read_file_to_buf(filepath, output_buf);
    if (body_len < 0) {
        /* 读取失败，回退为 500 */
        /* 简化处理：清空 buffer 重写 500 */
        output_buf->write_idx = 0;
        output_buf->read_idx  = 0;
        __build_status_line(output_buf, 500, 0);
        __build_error_body(output_buf, 500);
        return -1;
    }

    return 0;
}
