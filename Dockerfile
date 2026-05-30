# Flames Ready – Dockerfile
#
# Extends the existing Apache + PHP image to compile and install the
# Flames Ready extension, then configures PHP-FPM worker settings to
# match the persistent-worker model.
#
# Build:
#   docker build -t flames-ready .
#
# Or include in docker-compose.yml as a build context:
#   services:
#     php:
#       build: ./flames-ready

ARG PHP_VERSION=8.5
FROM php:${PHP_VERSION}-apache

USER root

# ── System dependencies ────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        gcc \
        make \
        libtool \
        pkg-config \
        re2c \
    && rm -rf /var/lib/apt/lists/*

# ── Copy extension source ──────────────────────────────────────────────
COPY config.m4              /usr/src/flames-ready/
COPY php_flames_ready.h     /usr/src/flames-ready/
COPY flames_ready_platform.h /usr/src/flames-ready/
COPY flames_ready_fcgi.h    /usr/src/flames-ready/
COPY flames_ready_fcgi.c    /usr/src/flames-ready/
COPY flames_ready.c         /usr/src/flames-ready/

# ── Build & install ────────────────────────────────────────────────────
RUN cd /usr/src/flames-ready \
    && phpize \
    && ./configure --enable-cflames-ready-service \
    && make -j"$(nproc)" \
    && make install

# ── Enable the extension ───────────────────────────────────────────────
RUN echo "extension=cflames_ready_service.so"      >  /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo ""                                      >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "; Flames Ready INI settings"           >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "; Enable worker mode (0 = Apache/FPM hooks, 1 = manual loop)" \
                                                 >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "flames_ready.worker_mode  = 0"         >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo ""                                      >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "; Call load callbacks only on the first request per worker"  \
                                                 >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "flames_ready.preload_once = 1"         >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo ""                                      >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "; Max requests per worker (0 = unlimited)"                   \
                                                 >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "flames_ready.max_requests = 0"         >> /usr/local/etc/php/conf.d/50-flames-ready.ini

# ── Recommended OPcache / worker settings ─────────────────────────────
RUN echo ""                                               >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "; OPcache – keep compiled bytecode in memory"  >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "opcache.enable           = 1"                  >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "opcache.memory_consumption = 256"              >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "opcache.interned_strings_buffer = 16"          >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "opcache.max_accelerated_files = 20000"         >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "opcache.validate_timestamps = 0"               >> /usr/local/etc/php/conf.d/50-flames-ready.ini \
 && echo "opcache.save_comments      = 1"                >> /usr/local/etc/php/conf.d/50-flames-ready.ini

# ── Enable Apache mod_rewrite (needed by most PHP frameworks) ──────────
RUN a2enmod rewrite

WORKDIR /var/www/html
