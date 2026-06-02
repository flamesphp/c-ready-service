/*
 * Flames Ready - PHP Extension
 * Persistent worker mode for PHP + Apache, inspired by FrankenPHP.
 */

#ifndef PHP_FLAMES_READY_H
#define PHP_FLAMES_READY_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "flames_ready_platform.h"
#include "flames_ready_fcgi.h"


extern zend_module_entry cflames_ready_service_module_entry;
#define phpext_cflames_ready_service_ptr &cflames_ready_service_module_entry

#define PHP_FLAMES_READY_VERSION "1.0.0"
#define PHP_FLAMES_READY_EXTNAME "cflames_ready_service"

/* Version embedded from config.yml at build time */
#define PHP_FLAMES_READY_SERVICE_VERSION "a3f8c2e1b7d94056f2a1c3e8b5d07f4a9c2e6b81"

#ifdef PHP_WIN32
#   define PHP_FLAMES_READY_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#   define PHP_FLAMES_READY_API __attribute__((visibility("default")))
#else
#   define PHP_FLAMES_READY_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

/* -----------------------------------------------------------------------
 * Callback entry: a class + static method pair stored in persistent memory.
 * ----------------------------------------------------------------------- */
typedef struct _flames_ready_callback {
    char   *class_name;
    size_t  class_len;
    char   *method_name;
    size_t  method_len;
} flames_ready_callback;

/* -----------------------------------------------------------------------
 * Module globals (persist for the entire life of a worker process).
 * ----------------------------------------------------------------------- */
ZEND_BEGIN_MODULE_GLOBALS(flames_ready)
    /* reset callbacks – called after each handled request */
    flames_ready_callback *reset_callbacks;
    int                    reset_count;
    int                    reset_cap;

    /* load callbacks – called once when the worker becomes ready */
    flames_ready_callback *load_callbacks;
    int                    load_count;
    int                    load_cap;

    /* state flags */
    bool       is_worker;      /* 1 = running inside a spawned worker      */
    bool       is_supervisor;  /* 1 = running as the supervisor process    */
    bool       initialized;    /* load callbacks already invoked          */
    bool       worker_mode;    /* INI: flames_ready_service.worker_mode   */
    bool       preload_once;   /* INI: flames_ready_service.preload_once  */
    zend_long  max_requests;   /* INI: flames_ready_service.max_requests  */
    zend_long  request_count;  /* total requests handled by this worker    */
    char      *socket_path;    /* INI: flames_ready_service.socket        */
    zend_long  workers;        /* INI: flames_ready_service.workers       */
    zend_long  worker_ttl;     /* INI: flames_ready_service.worker_ttl    */
    zend_long  worker_timeout; /* INI: flames_ready_service.worker_timeout*/
    /* passthrough mode – set by Worker::passthrough() */
    int            passthrough_mode;
    fr_socket_t    passthrough_conn_fd;
    uint16_t       passthrough_request_id;

    /* shared memory header – pointer valid in supervisor and all workers */
    void      *shared_header;
ZEND_END_MODULE_GLOBALS(flames_ready)

#define FLAMES_READY_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(flames_ready, v)

#if defined(ZTS) && defined(COMPILE_DL_CFLAMES_READY_SERVICE)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

/* -----------------------------------------------------------------------
 * Class entries
 * \Flames\Ready\Service
 * \Flames\Ready\Service\Worker
 * \Flames\Ready\Service\Supervisor
 * \Flames\Ready\Service\Register
 * \Flames\Ready\Service\Config
 * ----------------------------------------------------------------------- */
extern zend_class_entry *flames_ready_service_ce;
extern zend_class_entry *flames_ready_service_worker_ce;
extern zend_class_entry *flames_ready_service_supervisor_ce;
extern zend_class_entry *flames_ready_service_register_ce;
extern zend_class_entry *flames_ready_service_config_ce;

/* -----------------------------------------------------------------------
 * Arginfo – \Flames\Ready\Service
 * ----------------------------------------------------------------------- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Service_isReady, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* -----------------------------------------------------------------------
 * Arginfo – \Flames\Ready\Service\Worker
 * ----------------------------------------------------------------------- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Worker_isWorker, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Worker_getRequestCount, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Worker_passthrough, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Worker_getPids, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* -----------------------------------------------------------------------
 * Arginfo – \Flames\Ready\Service\Supervisor
 * ----------------------------------------------------------------------- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Supervisor_isSupervisor, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Supervisor_reloadWorkers, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

/* ?int – nullable = 1 */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Supervisor_getPid, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()

/* -----------------------------------------------------------------------
 * Method declarations
 * ----------------------------------------------------------------------- */

/* \Flames\Ready\Service */
PHP_METHOD(FlamesReadyService, isReady);

/* \Flames\Ready\Service\Worker */
PHP_METHOD(FlamesReadyServiceWorker, isWorker);
PHP_METHOD(FlamesReadyServiceWorker, getRequestCount);
PHP_METHOD(FlamesReadyServiceWorker, passthrough);
PHP_METHOD(FlamesReadyServiceWorker, getPids);

/* \Flames\Ready\Service\Supervisor */
PHP_METHOD(FlamesReadyServiceSupervisor, isSupervisor);
PHP_METHOD(FlamesReadyServiceSupervisor, reloadWorkers);
PHP_METHOD(FlamesReadyServiceSupervisor, getPid);

/* -----------------------------------------------------------------------
 * Arginfo – \Flames\Ready\Service\Register
 * ----------------------------------------------------------------------- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Register_load, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, class,  IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Register_reset, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, class,  IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Register_request, 0, 1, IS_LONG, 0)
    ZEND_ARG_INFO(0, handler)
ZEND_END_ARG_INFO()

/* -----------------------------------------------------------------------
 * Arginfo – \Flames\Ready\Service\Config
 * ----------------------------------------------------------------------- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Config_setWorkers, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, count, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* -----------------------------------------------------------------------
 * Method declarations – \Flames\Ready\Service\Register
 * ----------------------------------------------------------------------- */
PHP_METHOD(FlamesReadyServiceRegister, load);
PHP_METHOD(FlamesReadyServiceRegister, reset);
PHP_METHOD(FlamesReadyServiceRegister, request);

/* -----------------------------------------------------------------------
 * Method declarations – \Flames\Ready\Service\Config
 * ----------------------------------------------------------------------- */
PHP_METHOD(FlamesReadyServiceConfig, setWorkers);

/* -----------------------------------------------------------------------
 * Global function: __flames_c_ready_service_version__
 * ----------------------------------------------------------------------- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo___flames_c_ready_service_version__, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(__flames_c_ready_service_version__);

#endif /* PHP_FLAMES_READY_H */
