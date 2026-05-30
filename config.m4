dnl config.m4 - Flames Ready PHP Extension build configuration
dnl
dnl Usage:
dnl   phpize
dnl   ./configure --enable-cflames-ready-service
dnl   make
dnl   make install

PHP_ARG_ENABLE(
    [cflames_ready_service],
    [whether to enable Flames Ready Service support],
    [AS_HELP_STRING(
        [--enable-cflames-ready-service],
        [Enable Flames Ready Service persistent worker extension])],
    [no])

if test "$PHP_CFLAMES_READY_SERVICE" != "no"; then
    dnl ── PHP 8.5+ requirement ─────────────────────────────────────────────
    AC_MSG_CHECKING([for PHP version >= 8.5])
    php_version=$($PHP_CONFIG --version 2>/dev/null)
    AS_VERSION_COMPARE([$php_version], [8.5.0],
        [AC_MSG_ERROR([Flames Ready requires PHP >= 8.5 (found $php_version)])],
        [AC_MSG_RESULT([ok ($php_version)])],
        [AC_MSG_RESULT([ok ($php_version)])])

    AC_DEFINE(HAVE_FLAMES_READY, 1, [Whether Flames Ready is enabled])
    PHP_NEW_EXTENSION(cflames_ready_service, flames_ready.c flames_ready_fcgi.c, $ext_shared)
    dnl shm_open / shm_unlink are in librt on glibc < 2.17; harmless on newer
    PHP_ADD_LIBRARY(rt, 1, CFLAMES_READY_SERVICE_SHARED_LIBADD)
    PHP_SUBST(CFLAMES_READY_SERVICE_SHARED_LIBADD)
    PHP_ADD_MAKEFILE_FRAGMENT
fi
