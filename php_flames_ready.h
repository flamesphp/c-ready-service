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

typedef struct _flames_ready_callback {
    char   *class_name;
    size_t  class_len;
    char   *method_name;
    size_t  method_len;
} flames_ready_callback;

ZEND_BEGIN_MODULE_GLOBALS(flames_ready)
    flames_ready_callback *reset_callbacks;
    int                    reset_count;
    int                    reset_cap;

    flames_ready_callback *load_callbacks;
    int                    load_count;
    int                    load_cap;

    bool       is_worker;
    bool       is_supervisor;
    bool       initialized;
    bool       worker_mode;
    bool       preload_once;
    zend_long  max_requests;
    zend_long  request_count;
    char      *socket_path;
    zend_long  workers;
    zend_long  worker_ttl;
    zend_long  worker_timeout;
    int            passthrough_mode;
    fr_socket_t    passthrough_conn_fd;
    uint16_t       passthrough_request_id;

    void      *shared_header;
ZEND_END_MODULE_GLOBALS(flames_ready)

#define FLAMES_READY_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(flames_ready, v)

#if defined(ZTS) && defined(COMPILE_DL_CFLAMES_READY_SERVICE)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

extern zend_class_entry *flames_ready_service_ce;
extern zend_class_entry *flames_ready_service_worker_ce;
extern zend_class_entry *flames_ready_service_supervisor_ce;
extern zend_class_entry *flames_ready_service_register_ce;
extern zend_class_entry *flames_ready_service_config_ce;

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Service_isReady, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Supervisor_isSupervisor, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Supervisor_reloadWorkers, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Supervisor_getPid, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()

PHP_METHOD(FlamesReadyService, isReady);

PHP_METHOD(FlamesReadyServiceWorker, isWorker);
PHP_METHOD(FlamesReadyServiceWorker, getRequestCount);
PHP_METHOD(FlamesReadyServiceWorker, passthrough);
PHP_METHOD(FlamesReadyServiceWorker, getPids);

PHP_METHOD(FlamesReadyServiceSupervisor, isSupervisor);
PHP_METHOD(FlamesReadyServiceSupervisor, reloadWorkers);
PHP_METHOD(FlamesReadyServiceSupervisor, getPid);

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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo_Config_setWorkers, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, count, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(FlamesReadyServiceRegister, load);
PHP_METHOD(FlamesReadyServiceRegister, reset);
PHP_METHOD(FlamesReadyServiceRegister, request);

PHP_METHOD(FlamesReadyServiceConfig, setWorkers);

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(
    arginfo___flames_c_ready_service_version__, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(__flames_c_ready_service_version__);

#endif
