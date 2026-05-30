/*
 * flames_ready_fcgi.c
 *
 * Minimal FastCGI server used by xflames_ready_handle_request().
 *
 * Apache talks to this worker via mod_proxy_fcgi → Unix/TCP socket.
 * Because the PHP-CLI process never restarts, static class properties
 * and any PHP-level state survive across requests — exactly like
 * FrankenPHP's worker mode.
 *
 * Platform support: Linux, macOS, Windows (PHP 8.5+, WinSock2).
 * On Windows only TCP sockets are supported (no AF_UNIX).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "php.h"
#include "SAPI.h"
#include "php_variables.h"

#include "flames_ready_platform.h"
#include "flames_ready_fcgi.h"

/* =========================================================================
 * Internal FastCGI record layout
 * ========================================================================= */

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint8_t  requestIdB1;
    uint8_t  requestIdB0;
    uint8_t  contentLengthB1;
    uint8_t  contentLengthB0;
    uint8_t  paddingLength;
    uint8_t  reserved;
} fcgi_header_t;

/* =========================================================================
 * Low-level I/O helpers
 * ========================================================================= */

static int fcgi_read_exact(fr_socket_t fd, void *buf, size_t n)
{
    size_t  done = 0;
    ssize_t r;
    while (done < n) {
        r = fr_sock_read(fd, (char *)buf + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int fcgi_write_exact(fr_socket_t fd, const void *buf, size_t n)
{
    size_t  done = 0;
    ssize_t w;
    while (done < n) {
        w = fr_sock_write(fd, (const char *)buf + done, n - done);
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return 0;
}

/* =========================================================================
 * FastCGI name-value pair parser
 * ========================================================================= */

/*
 * Decode a FastCGI length field (1 or 4 bytes).
 * Advances *pos by the number of bytes consumed.
 * Returns -1 if the buffer is too short.
 */
static int fcgi_decode_len(
    const uint8_t *buf, size_t buf_len,
    size_t *pos, uint32_t *out)
{
    if (*pos >= buf_len) return -1;
    if ((buf[*pos] >> 7) == 0) {
        *out = buf[(*pos)++];
    } else {
        if (*pos + 4 > buf_len) return -1;
        *out = ((uint32_t)(buf[*pos]   & 0x7f) << 24)
             | ((uint32_t) buf[*pos+1]         << 16)
             | ((uint32_t) buf[*pos+2]         <<  8)
             |  (uint32_t) buf[*pos+3];
        *pos += 4;
    }
    return 0;
}

/*
 * Parse all name-value pairs from a FCGI_PARAMS content buffer
 * and insert them into the HashTable *ht.
 */
static void fcgi_parse_params(
    const uint8_t *buf, size_t len, HashTable *ht)
{
    size_t   pos = 0;
    uint32_t name_len, value_len;

    while (pos < len) {
        if (fcgi_decode_len(buf, len, &pos, &name_len)  < 0) break;
        if (fcgi_decode_len(buf, len, &pos, &value_len) < 0) break;
        if (pos + name_len + value_len > len)               break;

        const char *name  = (const char *)buf + pos;
        pos += name_len;
        const char *value = (const char *)buf + pos;
        pos += value_len;

        zval zv;
        ZVAL_STRINGL(&zv, value, value_len);
        zend_hash_str_update(ht, name, name_len, &zv);
    }
}

/* =========================================================================
 * Socket management
 * ========================================================================= */

fr_socket_t flames_ready_fcgi_open_socket(const char *path)
{
    fr_socket_t fd;

    /* Numeric string → TCP socket on 0.0.0.0:port */
    if (path[0] >= '0' && path[0] <= '9') {
        int port = atoi(path);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == FR_INVALID_SOCKET) return FR_INVALID_SOCKET;

#ifdef _WIN32
        BOOL opt = TRUE;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#  ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#  endif
#endif

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons((uint16_t)port);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fr_close_socket(fd); return FR_INVALID_SOCKET;
        }
    } else {
#ifdef _WIN32
        /* Unix domain sockets require Windows 10 1809+ and a valid FS path.
         * Fall back to an error: use a TCP port number on Windows. */
        fprintf(stderr,
            "[Flames Ready] Unix sockets are not supported on Windows. "
            "Set flames_ready_service.socket to a TCP port number (e.g. 9000).\n");
        fflush(stderr);
        return FR_INVALID_SOCKET;
#else
        /* Unix socket */
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == FR_INVALID_SOCKET) return FR_INVALID_SOCKET;

        fr_unlink(path); /* remove stale socket */

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fr_close_socket(fd); return FR_INVALID_SOCKET;
        }

        /* Allow Apache (running as a different user) to connect */
        chmod(path, 0777);
#endif
    }

    if (listen(fd, SOMAXCONN) < 0) {
        fr_close_socket(fd); return FR_INVALID_SOCKET;
    }

    return fd;
}

fr_socket_t flames_ready_fcgi_accept(fr_socket_t server_fd)
{
    fr_socket_t conn;
    do {
        conn = accept(server_fd, NULL, NULL);
    } while (conn == FR_INVALID_SOCKET && fr_socket_err() == FR_EINTR);
    return conn;
}

/* =========================================================================
 * Request reading
 * ========================================================================= */

int flames_ready_fcgi_read_request(fr_socket_t conn_fd, flames_ready_fcgi_request_t *req)
{
    fcgi_header_t hdr;
    int           done_params = 0, done_stdin = 0;

    req->request_id = 0;
    req->body       = NULL;
    req->body_len   = 0;
    array_init(&req->params);

    while (!done_params || !done_stdin) {
        if (fcgi_read_exact(conn_fd, &hdr, sizeof(hdr)) < 0) return -1;

        uint16_t content_len = ((uint16_t)hdr.contentLengthB1 << 8)
                             |  (uint16_t)hdr.contentLengthB0;
        uint16_t request_id  = ((uint16_t)hdr.requestIdB1 << 8)
                             |  (uint16_t)hdr.requestIdB0;
        uint8_t  padding     = hdr.paddingLength;

        if (req->request_id == 0) req->request_id = request_id;

        uint8_t *content = NULL;
        if (content_len > 0) {
            content = emalloc(content_len);
            if (fcgi_read_exact(conn_fd, content, content_len) < 0) {
                efree(content);
                return -1;
            }
        }
        if (padding > 0) {
            uint8_t pad[255];
            fcgi_read_exact(conn_fd, pad, padding);
        }

        switch (hdr.type) {
            case FCGI_BEGIN_REQUEST:
                if (content) efree(content);
                break;

            case FCGI_PARAMS:
                if (content_len == 0) {
                    done_params = 1;
                } else {
                    fcgi_parse_params(content, content_len,
                                      Z_ARRVAL(req->params));
                    efree(content);
                }
                break;

            case FCGI_STDIN:
                if (content_len == 0) {
                    done_stdin = 1;
                } else {
                    req->body = erealloc(req->body,
                                         req->body_len + content_len + 1);
                    memcpy(req->body + req->body_len, content, content_len);
                    req->body_len += content_len;
                    req->body[req->body_len] = '\0';
                    efree(content);
                }
                break;

            default:
                if (content) efree(content);
                break;
        }
    }

    return 0;
}

/* =========================================================================
 * Minimal URL-decode + query-string parser
 * ========================================================================= */

static char *fr_url_decode(const char *str, size_t len, size_t *out_len)
{
    char  *result = emalloc(len + 1);
    size_t j = 0, i;

    for (i = 0; i < len; i++) {
        if (str[i] == '%' && i + 2 < len) {
            char hi = str[i + 1], lo = str[i + 2];
            int  hi_v = (hi >= '0' && hi <= '9') ? hi - '0'
                      : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                      : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
            int  lo_v = (lo >= '0' && lo <= '9') ? lo - '0'
                      : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                      : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
            if (hi_v >= 0 && lo_v >= 0) {
                result[j++] = (char)((hi_v << 4) | lo_v);
                i += 2;
                continue;
            }
        } else if (str[i] == '+') {
            result[j++] = ' ';
            continue;
        }
        result[j++] = str[i];
    }
    result[j] = '\0';
    *out_len = j;
    return result;
}

static void fr_parse_query(const char *qs, size_t qs_len, zval *arr)
{
    const char *pos = qs;
    const char *end = qs + qs_len;

    while (pos < end) {
        const char *amp = (const char *)memchr(pos, '&', (size_t)(end - pos));
        if (!amp) amp = end;

        const char *eq = (const char *)memchr(pos, '=', (size_t)(amp - pos));

        size_t key_len, val_len;
        char  *key, *val;

        if (eq) {
            key = fr_url_decode(pos,      (size_t)(eq - pos),      &key_len);
            val = fr_url_decode(eq + 1,   (size_t)(amp - eq - 1),  &val_len);
        } else {
            key = fr_url_decode(pos,      (size_t)(amp - pos),     &key_len);
            val = estrndup("", 0);
            val_len = 0;
        }

        if (key_len > 0) {
            zval zv;
            ZVAL_STRINGL(&zv, val, val_len);
            zend_hash_str_update(Z_ARRVAL_P(arr), key, key_len, &zv);
        }

        efree(key);
        efree(val);
        pos = amp + 1;
    }
}

/* =========================================================================
 * Multipart/form-data helpers
 * ========================================================================= */

/* Binary-safe substring search (portable memmem). */
static const char *fr_memmem(const char *hay, size_t hlen,
                               const char *needle, size_t nlen)
{
    if (nlen == 0) return hay;
    if (hlen < nlen) return NULL;
    const char first = needle[0];
    const char *p    = hay;
    size_t rem       = hlen - nlen + 1;
    while (rem > 0) {
        const char *found = (const char *)memchr(p, first, rem);
        if (!found) return NULL;
        if (memcmp(found, needle, nlen) == 0) return found;
        rem -= (size_t)(found - p) + 1;
        p    = found + 1;
    }
    return NULL;
}

/* Case-insensitive substring search within hlen bytes. */
static const char *fr_memcasestr(const char *hay, size_t hlen,
                                   const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    if (hlen < nlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (fr_strncasecmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

/* Extract a parameter value from a header string. */
static char *fr_extract_param(const char *header, const char *param_name)
{
    size_t      plen = strlen(param_name);
    const char *p    = header;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (fr_strncasecmp(p, param_name, plen) == 0 && p[plen] == '=') {
            p += plen + 1;
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) end = p + strlen(p);
                return estrndup(p, (size_t)(end - p));
            } else {
                const char *end = p;
                while (*end && *end != ';' && *end != '\r' && *end != '\n') end++;
                while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
                return estrndup(p, (size_t)(end - p));
            }
        }
        while (*p && *p != ';') p++;
    }
    return NULL;
}

/*
 * Parse a multipart/form-data body.
 *
 * Text fields  → inserted into post_arr  ($_POST)
 * File fields  → written to temp files, entry inserted into files_arr ($_FILES)
 */
static void fr_parse_multipart(
    const char *body, size_t body_len,
    const char *boundary, size_t boundary_len,
    zval *post_arr, zval *files_arr)
{
    char  *delim     = emalloc(boundary_len + 3);
    memcpy(delim, "--", 2);
    memcpy(delim + 2, boundary, boundary_len);
    delim[boundary_len + 2] = '\0';
    size_t delim_len = boundary_len + 2;

    const char *body_end = body + body_len;

    const char *pos = fr_memmem(body, body_len, delim, delim_len);
    if (!pos) { efree(delim); return; }
    pos += delim_len;
    if (pos + 2 <= body_end && pos[0] == '\r' && pos[1] == '\n') pos += 2;
    else if (pos < body_end  && pos[0] == '\n')                   pos += 1;

    while (pos < body_end) {
        const char *hdr_end = fr_memmem(pos, (size_t)(body_end - pos),
                                         "\r\n\r\n", 4);
        if (!hdr_end) break;
        size_t      hdr_len  = (size_t)(hdr_end - pos);
        const char *part_body = hdr_end + 4;

        char *next_delim_str = emalloc(delim_len + 3);
        memcpy(next_delim_str, "\r\n", 2);
        memcpy(next_delim_str + 2, delim, delim_len);
        next_delim_str[delim_len + 2] = '\0';
        const char *next_boundary = fr_memmem(part_body,
                                               (size_t)(body_end - part_body),
                                               next_delim_str, delim_len + 2);
        efree(next_delim_str);

        size_t part_body_len = next_boundary
            ? (size_t)(next_boundary - part_body)
            : (size_t)(body_end - part_body);

        const char *cd = fr_memcasestr(pos, hdr_len, "content-disposition:");
        char *name     = NULL;
        char *filename = NULL;
        if (cd) {
            name     = fr_extract_param(cd, "name");
            filename = fr_extract_param(cd, "filename");
        }

        const char *ct_hdr  = fr_memcasestr(pos, hdr_len, "content-type:");
        char       *part_mime = NULL;
        if (ct_hdr) {
            ct_hdr += sizeof("content-type:") - 1;
            while (*ct_hdr == ' ' || *ct_hdr == '\t') ct_hdr++;
            const char *ct_end = fr_memmem(ct_hdr, (size_t)(hdr_end - ct_hdr),
                                            "\r\n", 2);
            if (!ct_end) ct_end = hdr_end;
            part_mime = estrndup(ct_hdr, (size_t)(ct_end - ct_hdr));
        }

        if (name) {
            if (filename) {
                /* ── File upload ──────────────────────────────────────────── */
                char tmp_path[PATH_MAX];
                int  tmp_fd     = -1;
                int  error_code = UPLOAD_ERR_OK;

#ifdef _WIN32
                char win_tmp_dir[MAX_PATH];
                const char *upload_dir = PG(upload_tmp_dir);
                if (!upload_dir || !upload_dir[0]) {
                    GetTempPathA(MAX_PATH, win_tmp_dir);
                    upload_dir = win_tmp_dir;
                }
                if (GetTempFileNameA(upload_dir, "php", 0, tmp_path) == 0) {
                    error_code  = UPLOAD_ERR_CANT_WRITE;
                    tmp_path[0] = '\0';
                } else {
                    tmp_fd = _open(tmp_path,
                                   _O_CREAT | _O_WRONLY | _O_BINARY | _O_TRUNC,
                                   _S_IREAD | _S_IWRITE);
                    if (tmp_fd < 0) {
                        error_code  = UPLOAD_ERR_CANT_WRITE;
                        tmp_path[0] = '\0';
                    }
                }
#else
                const char *upload_dir = PG(upload_tmp_dir);
                if (!upload_dir || !upload_dir[0]) upload_dir = "/tmp";
                snprintf(tmp_path, sizeof(tmp_path), "%s/phpXXXXXX", upload_dir);
                tmp_fd     = mkstemp(tmp_path);
                if (tmp_fd < 0) {
                    error_code  = UPLOAD_ERR_CANT_WRITE;
                    tmp_path[0] = '\0';
                }
#endif

                if (tmp_fd >= 0) {
#ifdef _WIN32
                    if (part_body_len > 0 &&
                        _write(tmp_fd, part_body, (unsigned int)part_body_len)
                                != (int)part_body_len) {
                        error_code = UPLOAD_ERR_CANT_WRITE;
                    }
                    _close(tmp_fd);
#else
                    if (part_body_len > 0 &&
                        write(tmp_fd, part_body, part_body_len)
                                != (ssize_t)part_body_len) {
                        error_code = UPLOAD_ERR_CANT_WRITE;
                    }
                    close(tmp_fd);
#endif
                }

                if (error_code == UPLOAD_ERR_OK && tmp_path[0]) {
                    if (!SG(rfc1867_uploaded_files)) {
                        ALLOC_HASHTABLE(SG(rfc1867_uploaded_files));
                        zend_hash_init(SG(rfc1867_uploaded_files), 5, NULL,
                                       ZVAL_PTR_DTOR, 0);
                    }
                    zend_string *tmp_zstr = zend_string_init(
                        tmp_path, strlen(tmp_path), 0);
                    zval tmp_zval;
                    ZVAL_STR_COPY(&tmp_zval, tmp_zstr);
                    zend_hash_add(SG(rfc1867_uploaded_files), tmp_zstr, &tmp_zval);
                    zend_string_release(tmp_zstr);
                }

                zval fe;
                array_init(&fe);
                add_assoc_string(&fe, "name",      filename);
                add_assoc_string(&fe, "full_path", filename);
                add_assoc_string(&fe, "type",
                    part_mime ? part_mime : "application/octet-stream");
                add_assoc_string(&fe, "tmp_name",  tmp_path);
                add_assoc_long(  &fe, "error",     (zend_long)error_code);
                add_assoc_long(  &fe, "size",      (zend_long)part_body_len);
                zend_hash_str_update(Z_ARRVAL_P(files_arr),
                                     name, strlen(name), &fe);
            } else {
                /* ── Text field → $_POST ──────────────────────────────────── */
                zval zv;
                ZVAL_STRINGL(&zv, part_body, part_body_len);
                zend_hash_str_update(Z_ARRVAL_P(post_arr),
                                     name, strlen(name), &zv);
            }
        }

        if (name)      efree(name);
        if (filename)  efree(filename);
        if (part_mime) efree(part_mime);

        if (!next_boundary) break;

        pos = next_boundary + 2 + delim_len;
        if (pos + 2 <= body_end && pos[0] == '-' && pos[1] == '-') break;
        if (pos + 2 <= body_end && pos[0] == '\r' && pos[1] == '\n') pos += 2;
        else if (pos < body_end && pos[0] == '\n')                    pos += 1;
    }

    efree(delim);
}

/* =========================================================================
 * PHP superglobal population
 * ========================================================================= */

void flames_ready_fcgi_populate_globals(flames_ready_fcgi_request_t *req)
{
    zval sv, gv, pv, cv, rv, fv;
    array_init(&sv);
    array_init(&gv);
    array_init(&pv);
    array_init(&cv);
    array_init(&rv);
    array_init(&fv);

    /* ── $_SERVER: all FastCGI params ───────────────────────────────── */
    zend_string *k;
    zval        *v;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL(req->params), k, v) {
        if (k && Z_TYPE_P(v) == IS_STRING) {
            zval copy;
            ZVAL_STR_COPY(&copy, Z_STR_P(v));
            zend_hash_update(Z_ARRVAL(sv), k, &copy);
        }
    } ZEND_HASH_FOREACH_END();

    /* ── $_GET ──────────────────────────────────────────────────────── */
    zval *qs_zv = zend_hash_str_find(Z_ARRVAL(req->params),
                                      "QUERY_STRING", sizeof("QUERY_STRING") - 1);
    if (qs_zv && Z_TYPE_P(qs_zv) == IS_STRING && Z_STRLEN_P(qs_zv) > 0) {
        fr_parse_query(Z_STRVAL_P(qs_zv), Z_STRLEN_P(qs_zv), &gv);
    }

    /* ── Content-Type ───────────────────────────────────────────────── */
    zval *ct_zv = zend_hash_str_find(Z_ARRVAL(req->params),
                                      "CONTENT_TYPE", sizeof("CONTENT_TYPE") - 1);
    const char *content_type = (ct_zv && Z_TYPE_P(ct_zv) == IS_STRING)
                               ? Z_STRVAL_P(ct_zv) : "";

    SG(request_info).content_type = content_type;

    /* ── $_POST and $_FILES ─────────────────────────────────────────── */
    if (req->body_len > 0) {
        if (fr_strncasecmp(content_type, "multipart/form-data", 19) == 0) {
            char *boundary = fr_extract_param(content_type, "boundary");
            if (boundary) {
                fr_parse_multipart(req->body, req->body_len,
                                   boundary, strlen(boundary),
                                   &pv, &fv);
                efree(boundary);
            }
        } else if (fr_strncasecmp(content_type,
                               "application/x-www-form-urlencoded",
                               sizeof("application/x-www-form-urlencoded") - 1) == 0) {
            fr_parse_query(req->body, req->body_len, &pv);
        }
    }

    /* ── php://input ────────────────────────────────────────────────────── */
    if (SG(request_info).request_body) {
        php_stream_close(SG(request_info).request_body);
        SG(request_info).request_body = NULL;
    }
    if (req->body_len > 0 && req->body) {
        php_stream *bs = php_stream_memory_create(TEMP_STREAM_DEFAULT);
        php_stream_write(bs, req->body, req->body_len);
        php_stream_rewind(bs);
        SG(request_info).request_body = bs;
    }

    /* ── $_COOKIE ───────────────────────────────────────────────────── */
    zval *hc_zv = zend_hash_str_find(Z_ARRVAL(req->params),
                                      "HTTP_COOKIE", sizeof("HTTP_COOKIE") - 1);
    if (hc_zv && Z_TYPE_P(hc_zv) == IS_STRING && Z_STRLEN_P(hc_zv) > 0) {
        const char *ck_p = Z_STRVAL_P(hc_zv);
        const char *ck_e = ck_p + Z_STRLEN_P(hc_zv);
        while (ck_p < ck_e) {
            while (ck_p < ck_e && *ck_p == ' ') ck_p++;
            const char *semi = (const char *)memchr(ck_p, ';',
                                                    (size_t)(ck_e - ck_p));
            if (!semi) semi = ck_e;
            const char *eq = (const char *)memchr(ck_p, '=',
                                                  (size_t)(semi - ck_p));
            if (eq) {
                size_t nlen = (size_t)(eq - ck_p);
                while (nlen > 0 && ck_p[nlen - 1] == ' ') nlen--;
                size_t vlen = (size_t)(semi - eq - 1);
                if (nlen > 0) {
                    size_t dk, dv;
                    char *dn  = fr_url_decode(ck_p,  nlen, &dk);
                    char *dvs = fr_url_decode(eq + 1, vlen, &dv);
                    zval zv;
                    ZVAL_STRINGL(&zv, dvs, dv);
                    zend_hash_str_update(Z_ARRVAL(cv), dn, dk, &zv);
                    efree(dn);
                    efree(dvs);
                }
            }
            ck_p = semi + 1;
        }
    }

    /* ── $_REQUEST = COOKIE + POST + GET ────────────────────────────── */
    zend_hash_merge(Z_ARRVAL(rv), Z_ARRVAL(cv), zval_add_ref, 0);
    zend_hash_merge(Z_ARRVAL(rv), Z_ARRVAL(pv), zval_add_ref, 0);
    zend_hash_merge(Z_ARRVAL(rv), Z_ARRVAL(gv), zval_add_ref, 0);

    zval_ptr_dtor(&PG(http_globals)[TRACK_VARS_COOKIE]);
    ZVAL_COPY(&PG(http_globals)[TRACK_VARS_COOKIE], &cv);

    zend_hash_str_update(&EG(symbol_table), "_SERVER",  sizeof("_SERVER")  - 1, &sv);
    zend_hash_str_update(&EG(symbol_table), "_GET",     sizeof("_GET")     - 1, &gv);
    zend_hash_str_update(&EG(symbol_table), "_POST",    sizeof("_POST")    - 1, &pv);
    zend_hash_str_update(&EG(symbol_table), "_COOKIE",  sizeof("_COOKIE")  - 1, &cv);
    zend_hash_str_update(&EG(symbol_table), "_REQUEST", sizeof("_REQUEST") - 1, &rv);
    zend_hash_str_update(&EG(symbol_table), "_FILES",   sizeof("_FILES")   - 1, &fv);

    SG(request_info).query_string   = qs_zv ? Z_STRVAL_P(qs_zv) : "";
    SG(request_info).content_length = (zend_long)req->body_len;
    SG(request_info).cookie_data    = hc_zv ? Z_STRVAL_P(hc_zv) : NULL;

    zval *rm_zv = zend_hash_str_find(Z_ARRVAL(req->params),
                                      "REQUEST_METHOD", sizeof("REQUEST_METHOD") - 1);
    SG(request_info).request_method = rm_zv ? Z_STRVAL_P(rm_zv) : "GET";

    zval *ru_zv = zend_hash_str_find(Z_ARRVAL(req->params),
                                      "REQUEST_URI", sizeof("REQUEST_URI") - 1);
    SG(request_info).request_uri = ru_zv ? Z_STRVAL_P(ru_zv) : "/";
}

/* =========================================================================
 * Response sending
 * ========================================================================= */

static int fcgi_write_record(fr_socket_t fd, uint8_t type, uint16_t req_id,
                              const char *data, uint16_t len)
{
    fcgi_header_t hdr;
    hdr.version         = FCGI_VERSION_1;
    hdr.type            = type;
    hdr.requestIdB1     = (req_id >> 8) & 0xff;
    hdr.requestIdB0     =  req_id       & 0xff;
    hdr.contentLengthB1 = (len   >> 8) & 0xff;
    hdr.contentLengthB0 =  len         & 0xff;
    hdr.paddingLength   = 0;
    hdr.reserved        = 0;

    if (fcgi_write_exact(fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (len > 0 && fcgi_write_exact(fd, data, len) < 0) return -1;
    return 0;
}

int flames_ready_fcgi_send_response(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *headers, size_t headers_len,
    const char *body,    size_t body_len)
{
    int rc = 0;

    const char *hp = headers;
    size_t      hl = headers_len;
    while (hl > 0 && rc == 0) {
        uint16_t chunk = (uint16_t)(hl > 65535 ? 65535 : hl);
        rc = fcgi_write_record(conn_fd, FCGI_STDOUT, request_id, hp, chunk);
        hp += chunk;
        hl -= chunk;
    }

    const char *bp = body;
    size_t      bl = body_len;
    while (bl > 0 && rc == 0) {
        uint16_t chunk = (uint16_t)(bl > 65535 ? 65535 : bl);
        rc = fcgi_write_record(conn_fd, FCGI_STDOUT, request_id, bp, chunk);
        bp += chunk;
        bl -= chunk;
    }

    fcgi_write_record(conn_fd, FCGI_STDOUT, request_id, NULL, 0);

    uint8_t end_body[8] = {0, 0, 0, rc == 0 ? 0 : 1,
                           FCGI_REQUEST_COMPLETE, 0, 0, 0};
    fcgi_write_record(conn_fd, FCGI_END_REQUEST, request_id,
                      (char *)end_body, sizeof(end_body));
    return rc;
}

/* =========================================================================
 * Streaming / passthrough helpers
 * ========================================================================= */

int flames_ready_fcgi_begin_stream(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *headers, size_t headers_len,
    const char *initial_body, size_t initial_body_len)
{
    int rc = 0;

    const char *hp = headers;
    size_t      hl = headers_len;
    while (hl > 0 && rc == 0) {
        uint16_t chunk = (uint16_t)(hl > 65535 ? 65535 : hl);
        rc = fcgi_write_record(conn_fd, FCGI_STDOUT, request_id, hp, chunk);
        hp += chunk;
        hl -= chunk;
    }

    const char *bp = initial_body;
    size_t      bl = initial_body_len;
    while (bl > 0 && rc == 0) {
        uint16_t chunk = (uint16_t)(bl > 65535 ? 65535 : bl);
        rc = fcgi_write_record(conn_fd, FCGI_STDOUT, request_id, bp, chunk);
        bp += chunk;
        bl -= chunk;
    }
    return rc;
}

int flames_ready_fcgi_send_chunk(
    fr_socket_t conn_fd, uint16_t request_id,
    const char *data, size_t data_len)
{
    if (!data || data_len == 0) return 0;
    int rc = 0;
    const char *p   = data;
    size_t      rem = data_len;
    while (rem > 0 && rc == 0) {
        uint16_t chunk = (uint16_t)(rem > 65535 ? 65535 : rem);
        rc = fcgi_write_record(conn_fd, FCGI_STDOUT, request_id, p, chunk);
        p   += chunk;
        rem -= chunk;
    }
    return rc;
}

int flames_ready_fcgi_end_stream(fr_socket_t conn_fd, uint16_t request_id)
{
    fcgi_write_record(conn_fd, FCGI_STDOUT, request_id, NULL, 0);
    uint8_t end_body[8] = {0, 0, 0, 0, FCGI_REQUEST_COMPLETE, 0, 0, 0};
    return fcgi_write_record(conn_fd, FCGI_END_REQUEST, request_id,
                              (char *)end_body, sizeof(end_body));
}

/* =========================================================================
 * Cleanup
 * ========================================================================= */

void flames_ready_fcgi_request_free(flames_ready_fcgi_request_t *req)
{
    zval_ptr_dtor(&req->params);
    if (req->body) {
        efree(req->body);
        req->body     = NULL;
        req->body_len = 0;
    }
}
