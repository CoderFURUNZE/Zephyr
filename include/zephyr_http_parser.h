#ifndef __ZEPHYR_HTTP_PARSER_H
#define __ZEPHYR_HTTP_PARSER_H

#include "../include/zephyr_buffer.h"

/* 静态文件根目录 */
#define DOCUMENT_ROOT   "www"

/**
 * @brief 有限状态机的解析状态枚举
 */
typedef enum {
    PARSE_REQUESTLINE,  /* 正在解析请求行 (Method, URL, Version)          */
    PARSE_HEADERS,      /* 正在解析请求头 (Key: Value)                    */
    PARSE_BODY,         /* 正在解析请求体 (GET 通常为空)                  */
    PARSE_PARSE_DONE    /* 解析完全结束                                   */
} zephyr_http_state_t;

/**
 * @brief HTTP 行解析的行状态枚举（辅助状态机）
 */
typedef enum {
    LINE_OK,            /* 读取到完整的一行（以 \r\n 结尾）               */
    LINE_BAD,           /* 行语法错误                                     */
    LINE_OPEN           /* 行数据不完整（半包），需要继续接收             */
} zephyr_line_state_t;

/**
 * @brief HTTP 请求结果结构体
 * @note  零拷贝设计，指针直接指向 Buffer 内部内存，不进行 strcpy
 */
typedef struct zephyr_http_request {
    char* method;                    /* 请求方法，如 "GET"                */
    char* url;                       /* 请求路径，如 "/index.html"        */
    char* version;                   /* 协议版本，如 "HTTP/1.1"           */
    int   keep_alive;                /* 是否保持长连接                    */
    zephyr_http_state_t state;       /* 当前主状态机所处位置              */
} zephyr_http_request_t;

/**
 * @brief 初始化 / 重置 HTTP 请求结构体
 */
void zephyr_http_init_request(zephyr_http_request_t* req);

/**
 * @brief 核心解析接口：驱动有限状态机，解析输入缓冲区中的数据
 * @param req       请求结构体指针（保存解析结果）
 * @param input_buf 输入动态缓冲区指针（数据源）
 * @return          当前状态，若返回 PARSE_PARSE_DONE 代表请求解析成功
 */
zephyr_http_state_t zephyr_http_parse_request(zephyr_http_request_t* req,
                                              zephyr_buffer_t* input_buf);

/**
 * @brief 业务响应接口：根据请求 URL 读取文件，组装完整 HTTP 响应
 *
 * 自动处理：
 *   - 文件存在 → 200 OK + Content-Length + Content-Type + 文件内容
 *   - 文件不存在 → 404 Not Found
 *   - 不支持的 HTTP 方法 → 405 Method Not Allowed
 *
 * @param req        解析好的请求结果
 * @param output_buf 输出动态缓冲区指针（组装好的数据存入这里）
 * @return          0 成功，负数失败
 */
int zephyr_http_make_response(zephyr_http_request_t* req,zephyr_buffer_t* output_buf);

#endif
