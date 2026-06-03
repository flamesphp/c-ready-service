#ifndef FLAMES_READY_FCGI_H
#define FLAMES_READY_FCGI_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "flames_ready_platform.h"

#define FCGI_VERSION_1         1

#define FCGI_BEGIN_REQUEST     1
#define FCGI_ABORT_REQUEST     2
#define FCGI_END_REQUEST       3
#define FCGI_PARAMS            4
#define FCGI_STDIN             5
#define FCGI_STDOUT            6
#define FCGI_STDERR            7

#define FCGI_RESPONDER         1
#define FCGI_REQUEST_COMPLETE  0

typedef struct {
    uint16_t  request_id;
    zval      params;
    char     *body;
    size_t    body_len;
} flames_ready_fcgi_request_t;

fr_socket_t flames_ready_fcgi_open_socket(const char *path);

fr_socket_t flames_ready_fcgi_accept(fr_socket_t server_fd);

int flames_ready_fcgi_read_request(fr_socket_t conn_fd, flames_ready_fcgi_request_t *req);

void flames_ready_fcgi_populate_globals(flames_ready_fcgi_request_t *req);

int flames_ready_fcgi_send_response(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *headers, size_t headers_len,
    const char *body,    size_t body_len);

void flames_ready_fcgi_request_free(flames_ready_fcgi_request_t *req);

int flames_ready_fcgi_begin_stream(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *headers, size_t headers_len,
    const char *initial_body, size_t initial_body_len);

int flames_ready_fcgi_send_chunk(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *data, size_t data_len);

int flames_ready_fcgi_end_stream(fr_socket_t conn_fd, uint16_t request_id);

#endif
