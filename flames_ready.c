/*
 * Flames Ready - PHP Extension
 *
 * Enables FrankenPHP-style persistent worker behaviour inside a
 * standard Apache + PHP-FPM (or mod_php) stack:
 *
 *   - \Flames\Ready\Ready\Service\Register::load('Class', 'method')
 *       Registers a static method that is called ONCE when the worker
 *       process becomes active (bootstraps the application).
 *
 *   - \Flames\Ready\Ready\Service\Register::reset('Class', 'method')
 *       Registers a static method that is called AFTER every handled
 *       request to wipe per-request state (globals, caches, etc.).
 *
 *   - \Flames\Ready\Ready\Service\Register::request(callable $handler): int
 *       Starts the persistent FastCGI supervisor/worker loop (blocking).
 *       Enters the worker loop: invokes load callbacks once, then loops
 *       calling $handler() and reset callbacks until $handler returns
 *       false or max_requests is reached.  Returns total requests handled.
 *
 * Lifecycle integration with Apache / PHP-FPM:
 *   RINIT  – load callbacks are invoked automatically on the first
 *            request of each worker process (when preload_once = On).
 *   RSHUTDOWN – reset callbacks are invoked automatically after every
 *               request in non-worker-mode.
 *
 * INI settings:
 *   flames_ready_service.worker_mode  = 0|1  (default 0)
 *   flames_ready_service.preload_once = 0|1  (default 1)
 *   flames_ready_service.max_requests = N    (default 0 = unlimited)
 *
 * Platform support: Linux, macOS, Windows (PHP 8.5+).
 *   On Windows the supervisor/worker multi-process model is replaced by
 *   a single-worker loop in the calling process (no fork available).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "flames_ready_platform.h"
#include "php_flames_ready.h"
#include "SAPI.h"
#include "zend_smart_str.h"
#include "zend_execute.h"

/* =========================================================================
 * Module globals
 * ========================================================================= */

ZEND_DECLARE_MODULE_GLOBALS(flames_ready)

/* =========================================================================
 * INI entries
 * ========================================================================= */

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY(
        "flames_ready_service.worker_mode",  "0",
        PHP_INI_ALL, OnUpdateBool,
        worker_mode, zend_flames_ready_globals, flames_ready_globals)
    STD_PHP_INI_ENTRY(
        "flames_ready_service.preload_once", "1",
        PHP_INI_ALL, OnUpdateBool,
        preload_once, zend_flames_ready_globals, flames_ready_globals)
    STD_PHP_INI_ENTRY(
        "flames_ready_service.max_requests", "0",
        PHP_INI_ALL, OnUpdateLong,
        max_requests, zend_flames_ready_globals, flames_ready_globals)
    STD_PHP_INI_ENTRY(
        "flames_ready_service.socket", "/var/run/flames-ready/worker.sock",
        PHP_INI_ALL, OnUpdateString,
        socket_path, zend_flames_ready_globals, flames_ready_globals)
    STD_PHP_INI_ENTRY(
        "flames_ready_service.workers", "0",
        PHP_INI_ALL, OnUpdateLong,
        workers, zend_flames_ready_globals, flames_ready_globals)
    STD_PHP_INI_ENTRY(
        "flames_ready_service.worker_ttl", "300",
        PHP_INI_ALL, OnUpdateLong,
        worker_ttl, zend_flames_ready_globals, flames_ready_globals)
    STD_PHP_INI_ENTRY(
        "flames_ready_service.worker_timeout", "900",
        PHP_INI_ALL, OnUpdateLong,
        worker_timeout, zend_flames_ready_globals, flames_ready_globals)
PHP_INI_END()

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void flames_ready_free_callbacks(
    flames_ready_callback **cbs,
    int *count,
    int *cap)
{
    int i;
    for (i = 0; i < *count; i++) {
        if ((*cbs)[i].class_name)  pefree((*cbs)[i].class_name,  1);
        if ((*cbs)[i].method_name) pefree((*cbs)[i].method_name, 1);
    }
    if (*cbs) pefree(*cbs, 1);
    *cbs   = NULL;
    *count = 0;
    *cap   = 0;
}

static void flames_ready_push_callback(
    flames_ready_callback **cbs,
    int *count,
    int *cap,
    const char *class_name,  size_t class_len,
    const char *method_name, size_t method_len)
{
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 8 : (*cap * 2);
        *cbs = perealloc(*cbs, sizeof(flames_ready_callback) * new_cap, 1);
        *cap = new_cap;
    }
    (*cbs)[*count].class_name  = pestrndup(class_name,  class_len,  1);
    (*cbs)[*count].class_len   = class_len;
    (*cbs)[*count].method_name = pestrndup(method_name, method_len, 1);
    (*cbs)[*count].method_len  = method_len;
    (*count)++;
}

static int flames_ready_invoke_callbacks(
    flames_ready_callback *cbs,
    int count,
    const char *type)
{
    int   i;
    zval  retval, callable, class_zv, method_zv;

    for (i = 0; i < count; i++) {
        array_init(&callable);
        ZVAL_STRINGL(&class_zv,  cbs[i].class_name,  cbs[i].class_len);
        ZVAL_STRINGL(&method_zv, cbs[i].method_name, cbs[i].method_len);
        add_next_index_zval(&callable, &class_zv);
        add_next_index_zval(&callable, &method_zv);

        if (call_user_function(NULL, NULL, &callable, &retval, 0, NULL)
                == FAILURE) {
            php_error_docref(NULL, E_WARNING,
                "Flames Ready: failed to invoke %s callback %s::%s",
                type,
                cbs[i].class_name,
                cbs[i].method_name);
            zval_ptr_dtor(&callable);
            return FAILURE;
        }

        zval_ptr_dtor(&retval);
        zval_ptr_dtor(&callable);
    }

    return SUCCESS;
}

/* =========================================================================
 * PHP functions
 * ========================================================================= */

/* =========================================================================
 * Shared-memory layout – supervisor + all workers see the same region.
 *
 *  [ fr_shared_header_t ][ fr_worker_slot_t × N ]
 *
 * ========================================================================= */
typedef struct {
    volatile uint32_t reload_gen;        /* incremented by Supervisor::reloadWorkers()  */
    volatile pid_t    supervisor_pid;    /* PID of the supervisor process               */
    volatile int32_t  num_workers;       /* number of worker slots allocated            */
    volatile int32_t  requested_workers; /* target worker count (Config::setWorkers)    */
    uint32_t          _pad;
} fr_shared_header_t;

typedef struct {
    volatile pid_t  pid;
    volatile time_t worker_started;  /* epoch when child was born         */
    volatile time_t request_started; /* epoch when current req began; 0=idle */
} fr_worker_slot_t;

/* =========================================================================
 * Worker accept loop
 * ========================================================================= */

static int flames_ready_handle_one(fr_socket_t conn_fd, zval *handler)
{
    flames_ready_fcgi_request_t req;
    if (flames_ready_fcgi_read_request(conn_fd, &req) < 0) {
        fr_close_socket(conn_fd);
        return -1;
    }

    flames_ready_fcgi_populate_globals(&req);

    FLAMES_READY_G(passthrough_mode)        = 0;
    FLAMES_READY_G(passthrough_conn_fd)     = conn_fd;
    FLAMES_READY_G(passthrough_request_id)  = req.request_id;

    php_output_discard_all();
    php_output_start_user(NULL, 0, PHP_OUTPUT_HANDLER_STDFLAGS);

    int fatal = 0;

    zval retval;
    ZVAL_UNDEF(&retval);

    zend_try {
        call_user_function(NULL, NULL, handler, &retval, 0, NULL);
    } zend_catch {
        fatal = 1;
    } zend_end_try();

    zval_ptr_dtor(&retval);

    int had_exception = 0;
    if (!fatal && EG(exception)) {
        had_exception = 1;
        zend_clear_exception();
    }

    if (!fatal) {
        zval swc_func, swc_ret;
        ZVAL_STRING(&swc_func, "session_write_close");
        call_user_function(CG(function_table), NULL, &swc_func, &swc_ret, 0, NULL);
        zval_ptr_dtor(&swc_func);
        zval_ptr_dtor(&swc_ret);
    }

    /* ── Passthrough path ─────────────────────────────────────────────── */
    if (FLAMES_READY_G(passthrough_mode)) {
        FLAMES_READY_G(passthrough_conn_fd) = FR_INVALID_SOCKET;

        if (!fatal) {
            php_output_end();
            flames_ready_fcgi_end_stream(conn_fd, req.request_id);
        } else {
            php_output_discard();
        }

        flames_ready_fcgi_request_free(&req);
        fr_close_socket(conn_fd);
        sapi_header_op(SAPI_HEADER_DELETE_ALL, NULL);
        SG(sapi_headers).http_response_code = 0;

        if (SG(request_info).request_body) {
            php_stream_close(SG(request_info).request_body);
            SG(request_info).request_body = NULL;
        }
        if (SG(rfc1867_uploaded_files)) {
            zval *uv;
            ZEND_HASH_FOREACH_VAL(SG(rfc1867_uploaded_files), uv) {
                if (Z_TYPE_P(uv) == IS_STRING) {
                    fr_unlink(Z_STRVAL_P(uv));
                }
            } ZEND_HASH_FOREACH_END();
            zend_hash_destroy(SG(rfc1867_uploaded_files));
            FREE_HASHTABLE(SG(rfc1867_uploaded_files));
            SG(rfc1867_uploaded_files) = NULL;
        }

        zend_unset_timeout();

        if (fatal) {
            fprintf(stderr,
                "[Flames Ready] worker pid %d fatal in passthrough – restarting\n",
                fr_getpid());
            fflush(stderr);
            _exit(1);
        }

        flames_ready_invoke_callbacks(
            FLAMES_READY_G(reset_callbacks),
            FLAMES_READY_G(reset_count), "reset");
        return 0;
    }

    /* ── Normal (buffered) path ─────────────────────────────────────── */
    zval ob_content;
    ZVAL_UNDEF(&ob_content);
    php_output_get_contents(&ob_content);
    php_output_discard();

    const char *body     = "";
    size_t      body_len = 0;
    if (!fatal && Z_TYPE(ob_content) == IS_STRING) {
        body     = Z_STRVAL(ob_content);
        body_len = Z_STRLEN(ob_content);
    }

    smart_str headers_buf = {0};

    if (fatal) {
        const char *msg = "Internal Server Error";
        body     = msg;
        body_len = strlen(msg);
        smart_str_appends(&headers_buf, "Status: 500\r\n");
        smart_str_appends(&headers_buf, "Content-Type: text/plain\r\n");
        smart_str_append_printf(&headers_buf, "Content-Length: %zu\r\n", body_len);
    } else {
        int status_code = SG(sapi_headers).http_response_code;
        if (status_code == 0) status_code = (had_exception ? 500 : 200);

        bool has_ct       = 0;
        bool has_location = 0;
        zend_llist_element *el;
        for (el = SG(sapi_headers).headers.head; el; el = el->next) {
            sapi_header_struct *sh = (sapi_header_struct *)el->data;
            if (fr_strncasecmp(sh->header, "Content-Type", 12) == 0) has_ct = 1;
            if (fr_strncasecmp(sh->header, "Location:",     9) == 0) has_location = 1;
        }

        if (has_location && status_code >= 200 && status_code < 300)
            status_code = 302;

        smart_str_append_printf(&headers_buf, "Status: %d\r\n", status_code);

        for (el = SG(sapi_headers).headers.head; el; el = el->next) {
            sapi_header_struct *sh = (sapi_header_struct *)el->data;
            smart_str_appendl(&headers_buf, sh->header, sh->header_len);
            smart_str_appendl(&headers_buf, "\r\n", 2);
        }
        if (!has_ct)
            smart_str_appends(&headers_buf,
                "Content-Type: text/html; charset=UTF-8\r\n");
        smart_str_append_printf(&headers_buf,
            "Content-Length: %zu\r\n", body_len);
    }

    smart_str_appends(&headers_buf, "\r\n");
    smart_str_0(&headers_buf);

    flames_ready_fcgi_send_response(
        conn_fd, req.request_id,
        ZSTR_VAL(headers_buf.s), ZSTR_LEN(headers_buf.s),
        body, body_len);

    smart_str_free(&headers_buf);
    zval_ptr_dtor(&ob_content);
    flames_ready_fcgi_request_free(&req);
    fr_close_socket(conn_fd);

    sapi_header_op(SAPI_HEADER_DELETE_ALL, NULL);
    SG(sapi_headers).http_response_code = 0;

    if (SG(request_info).request_body) {
        php_stream_close(SG(request_info).request_body);
        SG(request_info).request_body = NULL;
    }

    if (SG(rfc1867_uploaded_files)) {
        zval *uv;
        ZEND_HASH_FOREACH_VAL(SG(rfc1867_uploaded_files), uv) {
            if (Z_TYPE_P(uv) == IS_STRING) {
                fr_unlink(Z_STRVAL_P(uv));
            }
        } ZEND_HASH_FOREACH_END();
        zend_hash_destroy(SG(rfc1867_uploaded_files));
        FREE_HASHTABLE(SG(rfc1867_uploaded_files));
        SG(rfc1867_uploaded_files) = NULL;
    }

    zend_unset_timeout();

    if (fatal) {
        fprintf(stderr,
            "[Flames Ready] worker pid %d fatal error/exit – restarting\n",
            fr_getpid());
        fflush(stderr);
        _exit(1);
    }

    flames_ready_invoke_callbacks(
        FLAMES_READY_G(reset_callbacks),
        FLAMES_READY_G(reset_count),
        "reset");

    return 0;
}

static void flames_ready_worker_loop(fr_socket_t server_fd, zval *handler,
                                     fr_worker_slot_t *slot,
                                     time_t ttl_offset)
{
    FLAMES_READY_G(is_worker) = 1;

    zend_long max     = FLAMES_READY_G(max_requests);
    zend_long ttl     = FLAMES_READY_G(worker_ttl);
    zend_long handled = 0;

    FR_IGNORE_SIGPIPE();

    /* Set server_fd non-blocking so accept() returns immediately when another
     * worker claimed the connection (POSIX) or when no connections are ready. */
    fr_set_nonblocking(server_fd, 1);

    if (slot) {
        slot->pid             = (pid_t)fr_getpid();
        slot->worker_started  = time(NULL) - ttl_offset;
        slot->request_started = 0;
    }

#ifndef FR_NO_FORK
    fr_shared_header_t *shdr = (fr_shared_header_t *)FLAMES_READY_G(shared_header);
    uint32_t my_gen = shdr ? shdr->reload_gen : 0;
#endif

    fprintf(stderr,
        "[Flames Ready] worker pid %d started (ttl=%lds offset=%lds)\n",
        fr_getpid(), (long)ttl, (long)ttl_offset);
    fflush(stderr);

    if (!FLAMES_READY_G(initialized)) {
        flames_ready_invoke_callbacks(
            FLAMES_READY_G(load_callbacks),
            FLAMES_READY_G(load_count),
            "load");
        FLAMES_READY_G(initialized) = 1;
    }

    while (1) {
        if (max > 0 && handled >= max) break;

        if (slot) slot->request_started = 0;

        fr_socket_t conn_fd = FR_INVALID_SOCKET;
        while (conn_fd == FR_INVALID_SOCKET) {
            /* TTL check */
            if (ttl > 0 && slot) {
                time_t elapsed = time(NULL) - slot->worker_started;
                if (elapsed >= (time_t)ttl) {
                    fprintf(stderr,
                        "[Flames Ready] worker pid %d TTL expired"
                        " (%lds, ttl=%lds) – exiting\n",
                        fr_getpid(), (long)elapsed, (long)ttl);
                    fflush(stderr);
                    goto worker_exit;
                }
            }

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(server_fd, &rfds);
            struct timeval tv = {1, 0};
            int r = select(fr_select_nfds(server_fd), &rfds, NULL, NULL, &tv);
            if (r < 0) {
                int err = fr_socket_err();
                if (err == FR_EINTR) continue;
                goto worker_exit;
            }
            if (r == 0) continue;

            conn_fd = accept(server_fd, NULL, NULL);
            if (conn_fd == FR_INVALID_SOCKET) {
                int err = fr_socket_err();
                if (err == FR_EINTR || err == FR_EAGAIN || err == FR_EWOULDBLOCK) {
                    conn_fd = FR_INVALID_SOCKET;
                    continue;
                }
                fr_usleep_10ms();
                conn_fd = FR_INVALID_SOCKET;
            } else {
                /* Accepted socket must be blocking for read/write to work. */
                fr_set_nonblocking(conn_fd, 0);
            }
        }

        if (slot) slot->request_started = time(NULL);

        flames_ready_handle_one(conn_fd, handler);

        handled++;
        FLAMES_READY_G(request_count)++;

        if (ttl > 0 && slot) {
            time_t elapsed = time(NULL) - slot->worker_started;
            if (elapsed >= (time_t)ttl) {
                fprintf(stderr,
                    "[Flames Ready] worker pid %d TTL expired"
                    " (%lds, ttl=%lds, %ld reqs) – exiting after request\n",
                    fr_getpid(), (long)elapsed, (long)ttl, (long)handled);
                fflush(stderr);
                break;
            }
        }

#ifndef FR_NO_FORK
        if (shdr && shdr->reload_gen != my_gen) {
            fprintf(stderr,
                "[Flames Ready] worker pid %d reload requested"
                " (gen %u→%u) – exiting after request\n",
                fr_getpid(), my_gen, shdr->reload_gen);
            fflush(stderr);
            break;
        }
#endif
    }

worker_exit:
    if (slot) { slot->request_started = 0; slot->pid = 0; }
}

/* =========================================================================
 * POSIX named shared memory helpers
 * ========================================================================= */

#define FR_NAMED_SHM_MAX_WORKERS 512

typedef struct {
    volatile pid_t    supervisor_pid;
    volatile int32_t  num_workers;
    volatile int32_t  requested_workers;
    volatile uint32_t reload_gen;          /* external processes write here to trigger reload */
    volatile pid_t    worker_pids[FR_NAMED_SHM_MAX_WORKERS];
} fr_named_shm_t;

static const char *flames_ready_socket_path(void)
{
    const char *p = FLAMES_READY_G(socket_path);
#ifdef _WIN32
    /* On Windows, Unix socket paths are not supported; default to TCP */
    if (!p || !p[0] || p[0] == '/') return "9000";
#endif
    return (p && p[0]) ? p : "/var/run/flames-ready/worker.sock";
}

static void flames_ready_shm_name(const char *sock_path,
                                  char *buf, size_t buf_size)
{
    size_t len = 0;
#ifndef _WIN32
    buf[len++] = '/';
#endif
    buf[len++] = 'f';
    buf[len++] = 'r';
    buf[len++] = '_';
    for (const char *p = sock_path; *p && len < buf_size - 1; p++) {
        buf[len++] = (isalnum((unsigned char)*p) || *p == '-' || *p == '.')
                     ? *p : '_';
    }
    buf[len] = '\0';
}

/* ── Open named shm (read-only) ─────────────────────────────────────────── */
static fr_named_shm_t *flames_ready_named_shm_open_ro(void)
{
    char shm_name[256];
    flames_ready_shm_name(flames_ready_socket_path(), shm_name, sizeof(shm_name));

#ifdef _WIN32
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, shm_name);
    if (!h) return NULL;
    fr_named_shm_t *pub = (fr_named_shm_t *)MapViewOfFile(
        h, FILE_MAP_READ, 0, 0, sizeof(fr_named_shm_t));
    CloseHandle(h);
    return pub;
#else
    int fd = shm_open(shm_name, O_RDONLY, 0);
    if (fd < 0) return (fr_named_shm_t *)FR_SHM_FAILED;
    fr_named_shm_t *pub = (fr_named_shm_t *)mmap(
        NULL, sizeof(fr_named_shm_t), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return pub;
#endif
}

/* ── Open named shm (read-write) ─────────────────────────────────────────── */
static fr_named_shm_t *flames_ready_named_shm_open_rw(void)
{
    char shm_name[256];
    flames_ready_shm_name(flames_ready_socket_path(), shm_name, sizeof(shm_name));

#ifdef _WIN32
    HANDLE h = OpenFileMappingA(FILE_MAP_WRITE, FALSE, shm_name);
    if (!h) return NULL;
    fr_named_shm_t *pub = (fr_named_shm_t *)MapViewOfFile(
        h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fr_named_shm_t));
    CloseHandle(h);
    return pub;
#else
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) return (fr_named_shm_t *)FR_SHM_FAILED;
    fr_named_shm_t *pub = (fr_named_shm_t *)mmap(
        NULL, sizeof(fr_named_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return pub;
#endif
}

/* ── Helper: check if a named shm pointer represents failure ─────────────── */
static int fr_named_shm_is_failed(fr_named_shm_t *p)
{
#ifdef _WIN32
    return p == NULL;
#else
    return p == (fr_named_shm_t *)MAP_FAILED;
#endif
}

static void flames_ready_named_shm_update_workers(fr_named_shm_t *pub,
                                                   pid_t *pids, int n)
{
    int cap = n < FR_NAMED_SHM_MAX_WORKERS ? n : FR_NAMED_SHM_MAX_WORKERS;
    for (int i = 0; i < cap; i++) {
        pub->worker_pids[i] = pids[i] > 0 ? pids[i] : 0;
    }
}

/* =========================================================================
 * Output-buffer callback used in passthrough mode.
 * ========================================================================= */

static int flames_ready_stream_ob_handler(void **handler_context,
                                           php_output_context *output_context)
{
    if (output_context->in.data && output_context->in.used > 0) {
        fr_socket_t fd = FLAMES_READY_G(passthrough_conn_fd);
        uint16_t rid   = FLAMES_READY_G(passthrough_request_id);
        if (fd != FR_INVALID_SOCKET) {
            flames_ready_fcgi_send_chunk(fd, rid,
                output_context->in.data, output_context->in.used);
        }
    }
    output_context->out.data = NULL;
    output_context->out.used = 0;
    output_context->out.free = 0;
    return SUCCESS;
}

/* =========================================================================
 * PHP method: Worker::passthrough
 * ========================================================================= */

PHP_METHOD(FlamesReadyServiceWorker, passthrough)
{
    ZEND_PARSE_PARAMETERS_NONE();

    if (FLAMES_READY_G(passthrough_conn_fd) == FR_INVALID_SOCKET) {
        RETURN_NULL();
    }

    if (FLAMES_READY_G(passthrough_mode)) {
        RETURN_NULL();
    }

    zval ob_content;
    ZVAL_UNDEF(&ob_content);
    php_output_get_contents(&ob_content);
    php_output_discard();

    FLAMES_READY_G(passthrough_mode) = 1;

    fr_socket_t fd = FLAMES_READY_G(passthrough_conn_fd);
    uint16_t rid   = FLAMES_READY_G(passthrough_request_id);

    int status_code = SG(sapi_headers).http_response_code;
    if (status_code == 0) status_code = 200;

    smart_str hbuf = {0};
    smart_str_append_printf(&hbuf, "Status: %d\r\n", status_code);

    bool has_ct = 0;
    zend_llist_element *el;
    for (el = SG(sapi_headers).headers.head; el; el = el->next) {
        sapi_header_struct *sh = (sapi_header_struct *)el->data;
        if (fr_strncasecmp(sh->header, "Content-Type", 12) == 0) has_ct = 1;
        smart_str_appendl(&hbuf, sh->header, sh->header_len);
        smart_str_appendl(&hbuf, "\r\n", 2);
    }
    if (!has_ct)
        smart_str_appends(&hbuf, "Content-Type: text/html; charset=UTF-8\r\n");
    smart_str_appends(&hbuf, "\r\n");
    smart_str_0(&hbuf);

    const char *init_body     = "";
    size_t      init_body_len = 0;
    if (Z_TYPE(ob_content) == IS_STRING) {
        init_body     = Z_STRVAL(ob_content);
        init_body_len = Z_STRLEN(ob_content);
    }

    flames_ready_fcgi_begin_stream(fd, rid,
        ZSTR_VAL(hbuf.s), ZSTR_LEN(hbuf.s),
        init_body, init_body_len);

    smart_str_free(&hbuf);
    zval_ptr_dtor(&ob_content);

    php_output_handler *h = php_output_handler_create_internal(
        ZEND_STRL("flames_ready_stream"),
        flames_ready_stream_ob_handler,
        0,
        PHP_OUTPUT_HANDLER_STDFLAGS | PHP_OUTPUT_HANDLER_REMOVABLE);
    if (h) {
        php_output_handler_start(h);
    }

    RETURN_NULL();
}

/* =========================================================================
 * PHP method: Supervisor::reloadWorkers
 * ========================================================================= */

PHP_METHOD(FlamesReadyServiceSupervisor, reloadWorkers)
{
    ZEND_PARSE_PARAMETERS_NONE();

    fr_shared_header_t *shdr = (fr_shared_header_t *)FLAMES_READY_G(shared_header);
    if (shdr) {
        /* Called from within the supervisor process: update private shm directly */
        shdr->reload_gen++;

        /* Also mirror to named shm so the value stays consistent */
        fr_named_shm_t *pub = flames_ready_named_shm_open_rw();
        if (!fr_named_shm_is_failed(pub)) {
            pub->reload_gen = shdr->reload_gen;
            fr_shm_unmap(pub, sizeof(fr_named_shm_t));
        }

        fprintf(stderr,
            "[Flames Ready] reload requested by pid %d (gen→%u)\n",
            fr_getpid(), shdr->reload_gen);
    } else {
        /* Called from an external process: write to the named shm;
         * the supervisor will propagate to private shm on its next tick. */
        fr_named_shm_t *pub = flames_ready_named_shm_open_rw();
        if (fr_named_shm_is_failed(pub)) {
            /* No running supervisor – nothing to reload */
            RETURN_FALSE;
        }
        pub->reload_gen++;
        fprintf(stderr,
            "[Flames Ready] reload requested by external pid %d (pub gen→%u)\n",
            fr_getpid(), pub->reload_gen);
        fr_shm_unmap(pub, sizeof(fr_named_shm_t));
    }

    fflush(stderr);
    RETURN_NULL();
}

/* =========================================================================
 * PHP method: Register::request  (= the main entry point – starts the loop)
 * ========================================================================= */

static void flames_ready_start(zval *handler, zval *return_value);

PHP_METHOD(FlamesReadyServiceRegister, request)
{
    zval *handler;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    flames_ready_start(handler, return_value);
}

/* =========================================================================
 * flames_ready_start – starts the supervisor/worker loop (blocking)
 * ========================================================================= */

static void flames_ready_start(zval *handler, zval *return_value)
{
    if (!zend_is_callable(handler, 0, NULL)) {
        zend_throw_exception_ex(zend_ce_type_error, 0,
            "Flames Ready: argument must be callable");
        RETURN_FALSE;
    }

    const char *sock_path = flames_ready_socket_path();

    /* Open the FastCGI listening socket */
    fr_socket_t server_fd = FR_INVALID_SOCKET;
    int bind_tries = 30;
    while (bind_tries-- > 0) {
        server_fd = flames_ready_fcgi_open_socket(sock_path);
        if (server_fd != FR_INVALID_SOCKET) break;
        php_error_docref(NULL, E_WARNING,
            "Flames Ready: socket bind failed, retrying in 1s...");
        fr_sleep_s(1);
    }
    if (server_fd == FR_INVALID_SOCKET) {
        php_error_docref(NULL, E_ERROR,
            "Flames Ready: cannot bind FastCGI socket '%s' after retries",
            sock_path);
        RETURN_FALSE;
    }

    /* Derive the named-shm identifier from the socket path */
    char shm_name[256];
    flames_ready_shm_name(sock_path, shm_name, sizeof(shm_name));

#ifdef _WIN32
    /* ── Windows: single-worker mode ─────────────────────────────────────
     *
     * fork() is unavailable on Windows.  This process is simultaneously
     * the supervisor and the sole worker.  We publish our PID via a
     * Windows named file mapping so external code can inspect the state.
     */

    /* Create or open the named shared memory */
    HANDLE win_shm_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL,
        PAGE_READWRITE,
        0, (DWORD)sizeof(fr_named_shm_t),
        shm_name);

    if (!win_shm_handle) {
        fr_close_socket(server_fd);
        php_error_docref(NULL, E_ERROR,
            "Flames Ready: CreateFileMapping('%s') failed: error %lu",
            shm_name, (unsigned long)GetLastError());
        RETURN_FALSE;
    }

    BOOL already_exists = (GetLastError() == ERROR_ALREADY_EXISTS);

    fr_named_shm_t *pub = (fr_named_shm_t *)MapViewOfFile(
        win_shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fr_named_shm_t));

    if (!pub) {
        CloseHandle(win_shm_handle);
        fr_close_socket(server_fd);
        php_error_docref(NULL, E_ERROR,
            "Flames Ready: MapViewOfFile failed: error %lu",
            (unsigned long)GetLastError());
        RETURN_FALSE;
    }

    if (already_exists && pub->supervisor_pid > 0) {
        HANDLE hProc = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE,
            (DWORD)pub->supervisor_pid);
        if (hProc) {
            CloseHandle(hProc);
            UnmapViewOfFile(pub);
            CloseHandle(win_shm_handle);
            fr_close_socket(server_fd);
            php_error_docref(NULL, E_ERROR,
                "Flames Ready: supervisor already running (pid %d).",
                (int)pub->supervisor_pid);
            RETURN_FALSE;
        }
    }

    memset(pub, 0, sizeof(fr_named_shm_t));
    pub->supervisor_pid    = (pid_t)fr_getpid();
    pub->num_workers       = 1;
    pub->requested_workers = 1;
    pub->worker_pids[0]    = (pid_t)fr_getpid();

    FLAMES_READY_G(is_supervisor) = 1;
    FLAMES_READY_G(shared_header) = NULL; /* no anonymous shm on Windows */

    fprintf(stderr,
        "[Flames Ready] supervisor pid %d – 1 worker (single-process mode)"
        " on '%s' (Windows)\n",
        fr_getpid(), sock_path);
    fflush(stderr);

    /* Run the worker loop in this process */
    flames_ready_worker_loop(server_fd, handler, NULL, 0);

    UnmapViewOfFile(pub);
    CloseHandle(win_shm_handle);
    fr_close_socket(server_fd);

    RETURN_LONG(FLAMES_READY_G(request_count));

#else /* POSIX: multi-process supervisor + worker model ─────────────────── */

    /* Guard against duplicate supervisors via named shared memory (atomic) */
    int shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shm_fd < 0) {
        if (errno != EEXIST) {
            php_error_docref(NULL, E_ERROR,
                "Flames Ready: shm_open('%s') failed: %s",
                shm_name, strerror(errno));
            fr_close_socket(server_fd);
            RETURN_FALSE;
        }

        int probe_fd = shm_open(shm_name, O_RDWR, 0);
        pid_t existing_pid = 0;
        if (probe_fd >= 0) {
            fr_named_shm_t *probe = (fr_named_shm_t *)mmap(
                NULL, sizeof(fr_named_shm_t),
                PROT_READ, MAP_SHARED, probe_fd, 0);
            close(probe_fd);
            if (probe != (fr_named_shm_t *)MAP_FAILED) {
                existing_pid = probe->supervisor_pid;
                munmap(probe, sizeof(fr_named_shm_t));
            }
        }

        if (existing_pid > 0 && kill(existing_pid, 0) == 0) {
            php_error_docref(NULL, E_ERROR,
                "Flames Ready: supervisor already running (pid %d).",
                (int)existing_pid);
            fr_close_socket(server_fd);
            RETURN_FALSE;
        }

        fprintf(stderr,
            "[Flames Ready] stale named shm '%s' (pid %d gone) – reclaiming\n",
            shm_name, (int)existing_pid);
        fflush(stderr);
        shm_unlink(shm_name);

        shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (shm_fd < 0) {
            php_error_docref(NULL, E_ERROR,
                "Flames Ready: cannot reclaim named shm '%s': %s",
                shm_name, strerror(errno));
            fr_close_socket(server_fd);
            RETURN_FALSE;
        }
    }

    if (ftruncate(shm_fd, (off_t)sizeof(fr_named_shm_t)) < 0) {
        php_error_docref(NULL, E_ERROR,
            "Flames Ready: ftruncate named shm failed: %s", strerror(errno));
        shm_unlink(shm_name);
        close(shm_fd);
        fr_close_socket(server_fd);
        RETURN_FALSE;
    }

    fr_named_shm_t *pub = (fr_named_shm_t *)mmap(
        NULL, sizeof(fr_named_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (pub == (fr_named_shm_t *)MAP_FAILED) {
        php_error_docref(NULL, E_ERROR,
            "Flames Ready: mmap named shm failed: %s", strerror(errno));
        shm_unlink(shm_name);
        fr_close_socket(server_fd);
        RETURN_FALSE;
    }
    memset(pub, 0, sizeof(fr_named_shm_t));

    /* Determine worker count */
    int num_workers = (int)FLAMES_READY_G(workers);
    if (num_workers <= 0) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        num_workers = (cpus > 0) ? (int)(cpus * 4) : 16;
    }

    zend_long ttl     = FLAMES_READY_G(worker_ttl);
    zend_long timeout = FLAMES_READY_G(worker_timeout);

    /* Allocate anonymous shared memory: header + worker slots */
    size_t shm_size = sizeof(fr_shared_header_t)
                    + FR_NAMED_SHM_MAX_WORKERS * sizeof(fr_worker_slot_t);
    void *shm_base = mmap(NULL, shm_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm_base == MAP_FAILED) {
        php_error_docref(NULL, E_ERROR,
            "Flames Ready: mmap failed: %s", strerror(errno));
        fr_close_socket(server_fd);
        RETURN_FALSE;
    }
    memset(shm_base, 0, shm_size);

    fr_shared_header_t *shdr  = (fr_shared_header_t *)shm_base;
    fr_worker_slot_t   *slots = (fr_worker_slot_t *)((char *)shm_base
                                    + sizeof(fr_shared_header_t));

    FLAMES_READY_G(shared_header) = shdr;
    FLAMES_READY_G(is_supervisor) = 1;

    shdr->supervisor_pid    = getpid();
    shdr->num_workers       = (int32_t)num_workers;
    shdr->requested_workers = (int32_t)num_workers;

    pub->supervisor_pid    = getpid();
    pub->num_workers       = (int32_t)(num_workers < FR_NAMED_SHM_MAX_WORKERS
                                        ? num_workers : FR_NAMED_SHM_MAX_WORKERS);
    pub->requested_workers = pub->num_workers;

    /* Fork all initial workers */
    pid_t *pids = emalloc(FR_NAMED_SHM_MAX_WORKERS * sizeof(pid_t));
    memset(pids, 0, FR_NAMED_SHM_MAX_WORKERS * sizeof(pid_t));
    for (int i = 0; i < num_workers; i++) {
        time_t offset = (ttl > 0 && num_workers > 1)
            ? (time_t)ttl * i / num_workers
            : 0;
        pid_t pid = fork();
        if (pid < 0) {
            php_error_docref(NULL, E_WARNING,
                "Flames Ready: fork() failed for worker %d: %s",
                i, strerror(errno));
        } else if (pid == 0) {
            flames_ready_worker_loop(server_fd, handler, &slots[i], offset);
            _exit(0);
        } else {
            pids[i] = pid;
        }
    }

    flames_ready_named_shm_update_workers(pub, pids, num_workers);

    fprintf(stderr,
        "[Flames Ready] supervisor pid %d – %d worker(s) on '%s'"
        " (ttl=%lds timeout=%lds)\n",
        (int)getpid(), num_workers, sock_path,
        (long)ttl, (long)timeout);
    fflush(stderr);

    /* Supervisor loop */
    while (1) {
        sleep(1);
        time_t now = time(NULL);

        /* Propagate reload requests written by external processes */
        if (pub->reload_gen != shdr->reload_gen) {
            shdr->reload_gen = pub->reload_gen;
            fprintf(stderr,
                "[Flames Ready] supervisor propagated external reload (gen→%u)\n",
                shdr->reload_gen);
            fflush(stderr);
        }

        int target = (int)shdr->requested_workers;
        if (target < 1)                        target = 1;
        if (target > FR_NAMED_SHM_MAX_WORKERS) target = FR_NAMED_SHM_MAX_WORKERS;

        for (int i = target; i < num_workers; i++) {
            if (pids[i] > 0) kill(pids[i], SIGTERM);
        }

        if (target != num_workers) {
            num_workers = target;
            shdr->num_workers = (int32_t)num_workers;
            pub->num_workers  = (int32_t)num_workers;
        }

        if (timeout > 0) {
            for (int i = 0; i < num_workers; i++) {
                if (pids[i] <= 0) continue;
                time_t rs = slots[i].request_started;
                if (rs != 0 && (now - rs) >= (time_t)timeout) {
                    fprintf(stderr,
                        "[Flames Ready] worker pid %d stuck (%lds), killing\n",
                        (int)pids[i], (long)(now - rs));
                    fflush(stderr);
                    kill(pids[i], SIGKILL);
                }
            }
        }

        int   status;
        pid_t died;
        int   respawned = 0;
        while ((died = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < FR_NAMED_SHM_MAX_WORKERS; i++) {
                if (pids[i] != died) continue;
                slots[i].request_started = 0;
                slots[i].pid = 0;
                pids[i] = 0;
                if (i < num_workers) {
                    fprintf(stderr,
                        "[Flames Ready] worker pid %d exited – respawning\n",
                        (int)died);
                    fflush(stderr);
                    pid_t npid = fork();
                    if (npid == 0) {
                        flames_ready_worker_loop(server_fd, handler, &slots[i], 0);
                        _exit(0);
                    }
                    pids[i] = npid;
                    respawned = 1;
                } else {
                    fprintf(stderr,
                        "[Flames Ready] worker pid %d exited (scale-down)\n",
                        (int)died);
                    fflush(stderr);
                }
                break;
            }
        }

        for (int i = 0; i < num_workers; i++) {
            if (pids[i] <= 0) {
                pid_t npid = fork();
                if (npid == 0) {
                    flames_ready_worker_loop(server_fd, handler, &slots[i], 0);
                    _exit(0);
                }
                pids[i] = npid;
                respawned = 1;
            }
        }

        if (respawned) {
            flames_ready_named_shm_update_workers(pub, pids, num_workers);
        }
    }

    /* Unreachable in normal operation */
    munmap(pub, sizeof(fr_named_shm_t));
    shm_unlink(shm_name);
    FLAMES_READY_G(shared_header) = NULL;
    munmap(shm_base, shm_size);
    fr_close_socket(server_fd);
    efree(pids);
    RETURN_LONG(0);
#endif /* _WIN32 */
}

/* =========================================================================
 * Remaining PHP methods
 * ========================================================================= */

PHP_METHOD(FlamesReadyService, isReady)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(FLAMES_READY_G(initialized));
}

PHP_METHOD(FlamesReadyServiceWorker, getRequestCount)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(FLAMES_READY_G(request_count));
}

PHP_METHOD(FlamesReadyServiceWorker, isWorker)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(FLAMES_READY_G(is_worker));
}

PHP_METHOD(FlamesReadyServiceSupervisor, isSupervisor)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(FLAMES_READY_G(is_supervisor));
}

PHP_METHOD(FlamesReadyServiceWorker, getPids)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);

    fr_shared_header_t *shdr = (fr_shared_header_t *)FLAMES_READY_G(shared_header);
    if (shdr) {
        fr_worker_slot_t *slots = (fr_worker_slot_t *)((char *)shdr
                                    + sizeof(fr_shared_header_t));
        int32_t n = shdr->num_workers;
        for (int32_t i = 0; i < n; i++) {
            pid_t pid = slots[i].pid;
            if (pid > 0) add_next_index_long(return_value, (zend_long)pid);
        }
        return;
    }

    fr_named_shm_t *pub = flames_ready_named_shm_open_ro();
    if (fr_named_shm_is_failed(pub)) return;

    int32_t n = pub->num_workers;
    if (n > FR_NAMED_SHM_MAX_WORKERS) n = FR_NAMED_SHM_MAX_WORKERS;
    for (int32_t i = 0; i < n; i++) {
        pid_t pid = pub->worker_pids[i];
        if (pid > 0) add_next_index_long(return_value, (zend_long)pid);
    }
    fr_shm_unmap(pub, sizeof(fr_named_shm_t));
}

PHP_METHOD(FlamesReadyServiceSupervisor, getPid)
{
    ZEND_PARSE_PARAMETERS_NONE();

    pid_t pid = 0;

    fr_shared_header_t *shdr = (fr_shared_header_t *)FLAMES_READY_G(shared_header);
    if (shdr) {
        pid = shdr->supervisor_pid;
    } else {
        fr_named_shm_t *pub = flames_ready_named_shm_open_ro();
        if (!fr_named_shm_is_failed(pub)) {
            pid = pub->supervisor_pid;
            fr_shm_unmap(pub, sizeof(fr_named_shm_t));
        }
    }

    if (pid <= 0) RETURN_NULL();
    RETURN_LONG((zend_long)pid);
}

/* =========================================================================
 * \Flames\Ready\Ready\Service\Register
 * ========================================================================= */

PHP_METHOD(FlamesReadyServiceRegister, load)
{
    char   *class_name,  *method_name;
    size_t  class_len,    method_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(class_name, class_len)
        Z_PARAM_STRING(method_name, method_len)
    ZEND_PARSE_PARAMETERS_END();

    flames_ready_push_callback(
        &FLAMES_READY_G(load_callbacks),
        &FLAMES_READY_G(load_count),
        &FLAMES_READY_G(load_cap),
        class_name, class_len, method_name, method_len);

    if (!FLAMES_READY_G(worker_mode) && FLAMES_READY_G(preload_once)) {
        int idx = FLAMES_READY_G(load_count) - 1;
        flames_ready_invoke_callbacks(
            &FLAMES_READY_G(load_callbacks)[idx], 1, "load");
    }

    RETURN_TRUE;
}

PHP_METHOD(FlamesReadyServiceRegister, reset)
{
    char   *class_name,  *method_name;
    size_t  class_len,    method_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(class_name, class_len)
        Z_PARAM_STRING(method_name, method_len)
    ZEND_PARSE_PARAMETERS_END();

    flames_ready_push_callback(
        &FLAMES_READY_G(reset_callbacks),
        &FLAMES_READY_G(reset_count),
        &FLAMES_READY_G(reset_cap),
        class_name, class_len, method_name, method_len);

    RETURN_TRUE;
}

/* =========================================================================
 * \Flames\Ready\Ready\Service\Config
 * ========================================================================= */

PHP_METHOD(FlamesReadyServiceConfig, setWorkers)
{
    zend_long n;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(n)
    ZEND_PARSE_PARAMETERS_END();

    if (n < 1)                         n = 1;
    if (n > FR_NAMED_SHM_MAX_WORKERS)  n = FR_NAMED_SHM_MAX_WORKERS;

    fr_shared_header_t *shdr = (fr_shared_header_t *)FLAMES_READY_G(shared_header);
    if (shdr) {
        shdr->requested_workers = (int32_t)n;
    }

    fr_named_shm_t *pub = flames_ready_named_shm_open_rw();
    if (!fr_named_shm_is_failed(pub)) {
        pub->requested_workers = (int32_t)n;
        fr_shm_unmap(pub, sizeof(fr_named_shm_t));
    } else if (!shdr) {
        php_error_docref(NULL, E_WARNING,
            "Flames Ready: Config::setWorkers() – no supervisor running");
    }
}

/* =========================================================================
 * Global function: __flames_c_ready_service_version__
 * Returns the service version string embedded from config.yml at build time.
 * ========================================================================= */

PHP_FUNCTION(__flames_c_ready_service_version__)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(PHP_FLAMES_READY_SERVICE_VERSION);
}

/* =========================================================================
 * Module lifecycle
 * ========================================================================= */

PHP_GINIT_FUNCTION(flames_ready)
{
#if defined(COMPILE_DL_CFLAMES_READY_SERVICE) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    flames_ready_globals->reset_callbacks = NULL;
    flames_ready_globals->reset_count     = 0;
    flames_ready_globals->reset_cap       = 0;
    flames_ready_globals->load_callbacks  = NULL;
    flames_ready_globals->load_count      = 0;
    flames_ready_globals->load_cap        = 0;
    flames_ready_globals->is_worker        = 0;
    flames_ready_globals->is_supervisor    = 0;
    flames_ready_globals->initialized     = 0;
    flames_ready_globals->worker_mode     = 0;
    flames_ready_globals->preload_once    = 1;
    flames_ready_globals->max_requests    = 0;
    flames_ready_globals->request_count   = 0;
    flames_ready_globals->socket_path     = NULL;
    flames_ready_globals->workers               = 0;
    flames_ready_globals->worker_ttl            = 300;
    flames_ready_globals->worker_timeout        = 900;
    flames_ready_globals->passthrough_mode       = 0;
    flames_ready_globals->passthrough_conn_fd    = FR_INVALID_SOCKET;
    flames_ready_globals->passthrough_request_id = 0;
    flames_ready_globals->shared_header          = NULL;
}

PHP_GSHUTDOWN_FUNCTION(flames_ready)
{
    flames_ready_free_callbacks(
        &flames_ready_globals->reset_callbacks,
        &flames_ready_globals->reset_count,
        &flames_ready_globals->reset_cap);
    flames_ready_free_callbacks(
        &flames_ready_globals->load_callbacks,
        &flames_ready_globals->load_count,
        &flames_ready_globals->load_cap);
}

/* Forward declarations for method tables defined later in this file */
static const zend_function_entry flames_ready_service_methods[];
static const zend_function_entry flames_ready_service_worker_methods[];
static const zend_function_entry flames_ready_service_supervisor_methods[];
static const zend_function_entry flames_ready_service_register_methods[];
static const zend_function_entry flames_ready_service_config_methods[];

zend_class_entry *flames_ready_service_ce            = NULL;
zend_class_entry *flames_ready_service_worker_ce     = NULL;
zend_class_entry *flames_ready_service_supervisor_ce = NULL;
zend_class_entry *flames_ready_service_register_ce   = NULL;
zend_class_entry *flames_ready_service_config_ce     = NULL;

PHP_MINIT_FUNCTION(flames_ready)
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        php_error_docref(NULL, E_ERROR,
            "Flames Ready: WSAStartup failed");
        return FAILURE;
    }
#endif

    REGISTER_INI_ENTRIES();

    zend_class_entry ce_service;
    INIT_CLASS_ENTRY(ce_service, "Flames\\Ready\\Ready\\Service",
                     flames_ready_service_methods);
    flames_ready_service_ce = zend_register_internal_class(&ce_service);

    zend_class_entry ce_worker;
    INIT_CLASS_ENTRY(ce_worker, "Flames\\Ready\\Ready\\Service\\Worker",
                     flames_ready_service_worker_methods);
    flames_ready_service_worker_ce = zend_register_internal_class(&ce_worker);

    zend_class_entry ce_supervisor;
    INIT_CLASS_ENTRY(ce_supervisor, "Flames\\Ready\\Ready\\Service\\Supervisor",
                     flames_ready_service_supervisor_methods);
    flames_ready_service_supervisor_ce =
        zend_register_internal_class(&ce_supervisor);

    zend_class_entry ce_register;
    INIT_CLASS_ENTRY(ce_register, "Flames\\Ready\\Ready\\Service\\Register",
                     flames_ready_service_register_methods);
    flames_ready_service_register_ce = zend_register_internal_class(&ce_register);

    zend_class_entry ce_config;
    INIT_CLASS_ENTRY(ce_config, "Flames\\Ready\\Ready\\Service\\Config",
                     flames_ready_service_config_methods);
    flames_ready_service_config_ce = zend_register_internal_class(&ce_config);

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(flames_ready)
{
    UNREGISTER_INI_ENTRIES();
#ifdef _WIN32
    WSACleanup();
#endif
    return SUCCESS;
}

PHP_RINIT_FUNCTION(flames_ready)
{
#if defined(COMPILE_DL_CFLAMES_READY_SERVICE) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(flames_ready)
{
    if (!FLAMES_READY_G(worker_mode)) {
        flames_ready_invoke_callbacks(
            FLAMES_READY_G(reset_callbacks),
            FLAMES_READY_G(reset_count),
            "reset");
        FLAMES_READY_G(request_count)++;
        FLAMES_READY_G(initialized) = 1;
    }

    return SUCCESS;
}

PHP_MINFO_FUNCTION(flames_ready)
{
    char buf[32];

    php_info_print_table_start();
    php_info_print_table_header(2, "Flames Ready", "enabled");
    php_info_print_table_row(2, "Version",     PHP_FLAMES_READY_VERSION);
#ifdef _WIN32
    php_info_print_table_row(2, "Platform",    "Windows (single-worker mode)");
#else
    php_info_print_table_row(2, "Platform",    "POSIX (multi-process mode)");
#endif
    php_info_print_table_row(2, "Worker Mode",
        FLAMES_READY_G(worker_mode) ? "On" : "Off");

    if (FLAMES_READY_G(max_requests) == 0) {
        php_info_print_table_row(2, "Max Requests", "unlimited");
    } else {
        snprintf(buf, sizeof(buf), ZEND_LONG_FMT,
            FLAMES_READY_G(max_requests));
        php_info_print_table_row(2, "Max Requests", buf);
    }

    snprintf(buf, sizeof(buf), ZEND_LONG_FMT,
        FLAMES_READY_G(request_count));
    php_info_print_table_row(2, "Requests Handled", buf);
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

/* =========================================================================
 * Global function table
 * ========================================================================= */

static const zend_function_entry flames_ready_global_functions[] = {
    PHP_FE(__flames_c_ready_service_version__,
           arginfo___flames_c_ready_service_version__)
    PHP_FE_END
};

/* =========================================================================
 * Class entries and method tables
 * ========================================================================= */

static const zend_function_entry flames_ready_service_methods[] = {
    PHP_ME(FlamesReadyService, isReady,  arginfo_Service_isReady,  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry flames_ready_service_worker_methods[] = {
    PHP_ME(FlamesReadyServiceWorker, isWorker,        arginfo_Worker_isWorker,        ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesReadyServiceWorker, getRequestCount, arginfo_Worker_getRequestCount, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesReadyServiceWorker, passthrough,     arginfo_Worker_passthrough,     ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesReadyServiceWorker, getPids,         arginfo_Worker_getPids,         ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry flames_ready_service_supervisor_methods[] = {
    PHP_ME(FlamesReadyServiceSupervisor, isSupervisor,   arginfo_Supervisor_isSupervisor,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesReadyServiceSupervisor, reloadWorkers,  arginfo_Supervisor_reloadWorkers,  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesReadyServiceSupervisor, getPid,         arginfo_Supervisor_getPid,         ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry flames_ready_service_register_methods[] = {
    PHP_ME(FlamesReadyServiceRegister, load,    arginfo_Register_load,    ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesReadyServiceRegister, reset,   arginfo_Register_reset,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(FlamesReadyServiceRegister, request, arginfo_Register_request, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry flames_ready_service_config_methods[] = {
    PHP_ME(FlamesReadyServiceConfig, setWorkers, arginfo_Config_setWorkers, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

/* =========================================================================
 * Module entry
 * ========================================================================= */

zend_module_entry cflames_ready_service_module_entry = {
    STANDARD_MODULE_HEADER_EX,
    NULL,
    NULL,
    PHP_FLAMES_READY_EXTNAME,
    flames_ready_global_functions,
    PHP_MINIT(flames_ready),
    PHP_MSHUTDOWN(flames_ready),
    PHP_RINIT(flames_ready),
    PHP_RSHUTDOWN(flames_ready),
    PHP_MINFO(flames_ready),
    PHP_FLAMES_READY_VERSION,
    PHP_MODULE_GLOBALS(flames_ready),
    PHP_GINIT(flames_ready),
    PHP_GSHUTDOWN(flames_ready),
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_CFLAMES_READY_SERVICE
#   ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#   endif
ZEND_GET_MODULE(cflames_ready_service)
#endif
