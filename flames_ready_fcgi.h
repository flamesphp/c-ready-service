/*
 * flames_ready_fcgi.h
 * Minimal FastCGI server for the Flames Ready worker loop.
 */

#ifndef FLAMES_READY_FCGI_H
#define FLAMES_READY_FCGI_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "flames_ready_platform.h"

/* -------------------------------------------------------------------------
 * FastCGI protocol constants
 * ------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * Parsed FastCGI request
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t  request_id;
    zval      params;      /* HashTable of FCGI_PARAMS name => value (zstrings) */
    char     *body;        /* FCGI_STDIN contents (request body), may be NULL   */
    size_t    body_len;
} flames_ready_fcgi_request_t;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/*
 * Create and bind a FastCGI socket.
 * path may be:
 *   "/run/flames-ready.sock"   → AF_UNIX  (POSIX only)
 *   "9001"                     → TCP 0.0.0.0:9001
 *
 * Returns a listening fd on success, or FR_INVALID_SOCKET on error.
 */
fr_socket_t flames_ready_fcgi_open_socket(const char *path);

/*
 * Accept the next FastCGI connection on server_fd (blocking).
 * Returns the connected fd, or FR_INVALID_SOCKET on error.
 */
fr_socket_t flames_ready_fcgi_accept(fr_socket_t server_fd);

/*
 * Read a full FastCGI request from conn_fd into *req.
 * Caller must call flames_ready_fcgi_request_free() when done.
 * Returns 0 on success, -1 on error.
 */
int flames_ready_fcgi_read_request(fr_socket_t conn_fd, flames_ready_fcgi_request_t *req);

/*
 * Populate PHP superglobals ($_SERVER, $_GET, $_POST, $_COOKIE, $_REQUEST)
 * from the parsed FastCGI request.
 */
void flames_ready_fcgi_populate_globals(flames_ready_fcgi_request_t *req);

/*
 * Send the HTTP response (CGI-style: headers + blank line + body) as
 * FastCGI STDOUT records, followed by an END_REQUEST record.
 * headers: string with HTTP header lines separated by "\r\n" (no blank line)
 * body / body_len: response body
 */
int flames_ready_fcgi_send_response(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *headers, size_t headers_len,
    const char *body,    size_t body_len);

/*
 * Free resources allocated by flames_ready_fcgi_read_request().
 */
void flames_ready_fcgi_request_free(flames_ready_fcgi_request_t *req);

/* ── Streaming / passthrough API ─────────────────────────────────────────── */

/*
 * Begin a streaming response: sends HTTP headers + optional initial body.
 * Does NOT send the empty FCGI_STDOUT terminator; the connection stays open.
 * Follow with flames_ready_fcgi_send_chunk() and flames_ready_fcgi_end_stream().
 */
int flames_ready_fcgi_begin_stream(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *headers, size_t headers_len,
    const char *initial_body, size_t initial_body_len);

/*
 * Send a body chunk for an active streaming response.
 */
int flames_ready_fcgi_send_chunk(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *data, size_t data_len);

/*
 * Terminate a streaming response: sends empty FCGI_STDOUT + END_REQUEST.
 */
int flames_ready_fcgi_end_stream(fr_socket_t conn_fd, uint16_t request_id);

#endif /* FLAMES_READY_FCGI_H */
