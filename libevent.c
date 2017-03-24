/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2008 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Antony Dovgal <tony@daylessday.org>                          |
  |         Arnaud Le Blanc <lbarnaud@php.net>                           |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_streams.h"
#include "php_network.h"
#include "php_libevent.h"

#include <signal.h>

#if PHP_VERSION_ID >= 50301 && (HAVE_SOCKETS || defined(COMPILE_DL_SOCKETS))
# include "ext/sockets/php_sockets.h"
# define LIBEVENT_SOCKETS_SUPPORT
#endif

#ifdef PHP_WIN32
/* XXX compiling with 2.x on Windows. Luckily the ext code works thanks to the
compat exports from the libevent. However it might need to be adapted to the
never version, so this ifdefs would go away. */
# include <event2/event.h>
# include <event2/event_compat.h>
# include <event2/event_struct.h>
# include <event2/bufferevent.h>
# include <event2/bufferevent_compat.h>
#else
# include <event.h>
#endif

static int le_event_base;
static int le_event;
static int le_bufferevent;

#ifdef COMPILE_DL_LIBEVENT
ZEND_GET_MODULE(libevent)
#endif

typedef struct _php_event_base_t { /* {{{ */
	struct event_base *base;
	zval *rsrc_id;
	zend_uintptr_t events;
} php_event_base_t;
/* }}} */

typedef struct _php_event_callback_t { /* {{{ */
	zval func;
	zval arg;
} php_event_callback_t;
/* }}} */

typedef struct _php_event_t { /* {{{ */
	struct event *event;
	zval *rsrc_id;
	zval stream_id;
	php_event_base_t *base;
	php_event_callback_t *callback;
#ifdef ZTS
	void ***thread_ctx;
#endif
} php_event_t;
/* }}} */

typedef struct _php_bufferevent_t { /* {{{ */
	struct bufferevent *bevent;
	zval *rsrc_id;
	php_event_base_t *base;
	zval readcb;
	zval writecb;
	zval errorcb;
	zval arg;
#ifdef ZTS
	void ***thread_ctx;
#endif
} php_bufferevent_t;
/* }}} */

#define ZVAL_TO_BASE(zval) \
	(php_event_base_t *)zend_fetch_resource2_ex(zval, "event base", le_event_base, le_event_base)

#define ZVAL_TO_EVENT(zval) \
	(php_event_t *)zend_fetch_resource2_ex(zval, "event", le_event, le_event)

#define ZVAL_TO_BEVENT(zval) \
	(php_bufferevent_t *)zend_fetch_resource2_ex(zval, "buffer event", le_bufferevent, le_bufferevent)

/* {{{ internal funcs */

static inline void _php_event_callback_free(php_event_callback_t *callback) /* {{{ */
{
	if (!callback) {
		return;
	}
	zval_ptr_dtor(&callback->func);
	zval_ptr_dtor(&callback->arg);
	safe_efree(callback);
}
/* }}} */

ZEND_RSRC_DTOR_FUNC(_php_event_base_dtor) /* {{{ */
{
	if (!res || !res->ptr) {
		return;
	}
	php_event_base_t *base = (php_event_base_t*)res->ptr;
	if (!base)
		return;

	if (base->base)
		event_base_free(base->base);
	safe_efree(base);
}
/* }}} */

ZEND_RSRC_DTOR_FUNC(_php_event_dtor) /* {{{ */
{
	if (!res || !res->ptr) {
		return;
	}

	php_event_t *event = (php_event_t*)res->ptr;
	zval *base_id = NULL;

	if (!event) return;

	if (event->base) {
		base_id = event->base->rsrc_id;
		--event->base->events;
	}
	if (Z_TYPE_P(&event->stream_id) != IS_NULL) {
		zend_list_delete(Z_RES_P(&event->stream_id));
	}

	_php_event_callback_free(event->callback);


	event_del(event->event);
	safe_efree(event->event);
	safe_efree(event);

	if (base_id) {
		zend_list_delete(Z_RES_P(base_id));
	}
}
/* }}} */

ZEND_RSRC_DTOR_FUNC(_php_bufferevent_dtor) /* {{{ */
{

	if (!res || !res->ptr) {
		return;
	}

	php_bufferevent_t *bevent = (php_bufferevent_t*)res->ptr;
	zval *base_id = NULL;

	if (!bevent)
		return;

	zval_ptr_dtor(&bevent->readcb);
	zval_ptr_dtor(&bevent->writecb);
	zval_ptr_dtor(&bevent->errorcb);
	zval_ptr_dtor(&bevent->arg);
	bufferevent_free(bevent->bevent);

	if (bevent->base) {
		base_id = bevent->base->rsrc_id;
		--bevent->base->events;

		if (base_id) {
			zend_list_delete(Z_RES_P(base_id));
		}
	}

	safe_efree(bevent);
}
/* }}} */

static void _php_event_callback(int fd, short events, void *arg) /* {{{ */
{
	zval args[3];
	php_event_t *event = (php_event_t *)arg;
	php_event_callback_t *callback;
	zval retval;
	TSRMLS_FETCH_FROM_CTX(event ? event->thread_ctx : NULL);

	if (!event || !event->callback || !event->base) {
		return;
	}

	callback = event->callback;

	if (Z_TYPE_P(&event->stream_id) != IS_NULL) {
		args[0] = event->stream_id;
		Z_ADDREF_P(&args[0]); /* we do refcount-- later in zval_ptr_dtor */
	} else if (events & EV_SIGNAL) {
		ZVAL_LONG(&args[0], (zend_long)fd);
	} else {
		ZVAL_NULL(&args[0]);
	}

	ZVAL_LONG(&args[1], (zend_long)events);

	args[2] = callback->arg;
	if (Z_TYPE_P(&args[2]) != IS_NULL) {
		Z_TRY_ADDREF_P(&args[2]);
	}
	

	if (call_user_function(EG(function_table), NULL, &callback->func, &retval, 3, args) == SUCCESS) {
		zval_dtor(&retval);
	}


	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);
	zval_ptr_dtor(&args[2]);
	
}
/* }}} */

static void _php_bufferevent_readcb(struct bufferevent *be, void *arg) /* {{{ */
{
	zval args[2];
	zval retval;
	php_bufferevent_t *bevent = (php_bufferevent_t *)arg;
	TSRMLS_FETCH_FROM_CTX(bevent ? bevent->thread_ctx : NULL);

	if (!bevent || !bevent->base || Z_ISUNDEF(bevent->readcb)) {
		return;
	}

	
	ZVAL_COPY(&args[0], bevent->rsrc_id);  /* we do refcount-- later in zval_ptr_dtor */

	args[1] = bevent->arg;
	if (Z_TYPE_P(&args[1]) != IS_NULL) {
		Z_TRY_ADDREF_P(&args[1]);
	}

	if (call_user_function(EG(function_table), NULL, &bevent->readcb, &retval, 2, args) == SUCCESS) {
		zval_dtor(&retval);
	}

	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);

}
/* }}} */

static void _php_bufferevent_writecb(struct bufferevent *be, void *arg) /* {{{ */
{
	zval args[2];
	zval retval;
	php_bufferevent_t *bevent = (php_bufferevent_t *)arg;
	TSRMLS_FETCH_FROM_CTX(bevent ? bevent->thread_ctx : NULL);

	if (!bevent || !bevent->base || Z_ISUNDEF(bevent->writecb)) {
		return;
	}

	ZVAL_COPY(&args[0], bevent->rsrc_id);  /* we do refcount-- later in zval_ptr_dtor */

	args[1] = bevent->arg;
	if (Z_TYPE_P(&args[1]) != IS_NULL) {
		Z_TRY_ADDREF_P(&args[1]);
	}

	if (call_user_function(EG(function_table), NULL, &bevent->writecb, &retval, 2, args) == SUCCESS) {
		zval_dtor(&retval);
	}

	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);
	
}
/* }}} */

static void _php_bufferevent_errorcb(struct bufferevent *be, short what, void *arg) /* {{{ */
{
	zval args[3];
	zval retval;
	int args_size = 2;
	php_bufferevent_t *bevent = (php_bufferevent_t *)arg;
	TSRMLS_FETCH_FROM_CTX(bevent ? bevent->thread_ctx : NULL);

	if (!bevent || !bevent->base || Z_ISUNDEF(bevent->errorcb)) {
		return;
	}

	if (!bevent->rsrc_id)
		return;

	ZVAL_COPY(&args[0], bevent->rsrc_id); /* we do refcount-- later in zval_ptr_dtor */

	ZVAL_LONG(&args[1], (zend_long)what);
	args[2] = bevent->arg;
	if (Z_TYPE_P(&args[2]) != IS_NULL) {
		Z_TRY_ADDREF_P(&args[2]);
	}


	if (call_user_function(EG(function_table), NULL, &bevent->errorcb, &retval, 3, args) == SUCCESS) {
		zval_dtor(&retval);
	}

	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);
	zval_ptr_dtor(&args[2]);
}
/* }}} */

/* }}} */


/* {{{ proto resource event_base_new() 
 */
static PHP_FUNCTION(event_base_new)
{
	php_event_base_t *base;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "") != SUCCESS) {
		return;
	}

	base = emalloc(sizeof(php_event_base_t));
	base->rsrc_id = NULL;
	base->base = event_base_new();
	if (!base->base) {
		safe_efree(base);
		RETURN_FALSE;
	}

	base->events = 0;

	base->rsrc_id = zend_list_insert(base, le_event_base);
	ZVAL_COPY_VALUE(return_value, base->rsrc_id);
	Z_ADDREF_P(base->rsrc_id);
}
/* }}} */

/* {{{ proto bool event_base_reinit()
 */
static PHP_FUNCTION(event_base_reinit) {
    zval *zbase;
    php_event_base_t *base;
    int r = 0;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &zbase) != SUCCESS) {
        return;
    }

    if (!(base = ZVAL_TO_BASE(zbase)))
		RETURN_FALSE;

    r = event_reinit(base->base);
    if (r == -1) {
        RETURN_FALSE
    } else {
        RETURN_TRUE;
    }
}
/* }}} */

/* {{{ proto void event_base_free(resource base)
 *     return type is defined void at http://php.net/manual/en/function.event-base-free.php
 */
static PHP_FUNCTION(event_base_free)
{
	zval *zbase;
	php_event_base_t *base;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &zbase) != SUCCESS) {
		return;
	}

	if (!(base = ZVAL_TO_BASE(zbase)))
		return;

	if (base->events > 0) {
		php_error_docref(NULL, E_WARNING, "base has events attached to it and cannot be freed");
		RETURN_FALSE;
	}

	zend_list_close(Z_RES_P(base->rsrc_id));
}
/* }}} */

/* {{{ proto int event_base_loop(resource base[, int flags]) 
 */
static PHP_FUNCTION(event_base_loop)
{
	zval *zbase;
	php_event_base_t *base;
	zend_long flags = 0;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r|l", &zbase, &flags) != SUCCESS) {
		return;
	}

	base = ZVAL_TO_BASE(zbase);
	Z_ADDREF_P(base->rsrc_id); /* make sure the base cannot be destroyed during the loop */
	ret = event_base_loop(base->base, (int)flags);

	RETURN_LONG(ret);
}
/* }}} */

/* {{{ proto bool event_base_loopbreak(resource base) 
 */
static PHP_FUNCTION(event_base_loopbreak)
{
	zval *zbase;
	php_event_base_t *base;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &zbase) != SUCCESS) {
		return;
	}

	base = ZVAL_TO_BASE(zbase);
	ret = event_base_loopbreak(base->base);
	if (ret == 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool event_base_loopexit(resource base[, int timeout]) 
 */
static PHP_FUNCTION(event_base_loopexit)
{
	zval *zbase;
	php_event_base_t *base;
	int ret;
	zend_long timeout = -1;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r|l", &zbase, &timeout) != SUCCESS) {
		return;
	}

    if (!(base = ZVAL_TO_BASE(zbase)))
		RETURN_FALSE;

	if (timeout < 0) {
		ret = event_base_loopexit(base->base, NULL);
	} else {
		struct timeval time;
		
		time.tv_usec = timeout % 1000000;
		time.tv_sec = (long)timeout / 1000000;
		ret = event_base_loopexit(base->base, &time);
	}

	if (ret == 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool event_base_set(resource event, resource base) 
 */
static PHP_FUNCTION(event_base_set)
{
	zval *zbase, *zevent;
	php_event_base_t *base, *old_base;
	php_event_t *event;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rr", &zevent, &zbase) != SUCCESS) {
		return;
	}

	if (!(base = ZVAL_TO_BASE(zbase)))
		RETURN_FALSE;
	if (!(event = ZVAL_TO_EVENT(zevent)))
		RETURN_FALSE;

	old_base = event->base;
	ret = event_base_set(base->base, event->event);

	if (ret == 0) {
		if (base != old_base) {
			/* make sure the base is destroyed after the event */
			Z_ADDREF_P(base->rsrc_id);
			++base->events;

			/* deference the event from the old base */
			if (old_base) {
				--old_base->events;
				if (old_base->rsrc_id != NULL) {
					zend_list_delete(Z_RES_P(old_base->rsrc_id));
				}
			}
		}

		event->base = base;
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool event_base_priority_init(resource base, int npriorities) 
 */
static PHP_FUNCTION(event_base_priority_init)
{
	zval *zbase;
	php_event_base_t *base;
	zend_long npriorities;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rl", &zbase, &npriorities) != SUCCESS) {
		return;
	}

    if (!(base = ZVAL_TO_BASE(zbase)))
		RETURN_FALSE;

	if (npriorities < 0) {
		php_error_docref(NULL, E_WARNING, "npriorities cannot be less than zero");
		RETURN_FALSE;
	}

	ret = event_base_priority_init(base->base, npriorities);
	if (ret == 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */


/* {{{ proto resource event_new() 
 */
static PHP_FUNCTION(event_new)
{
	php_event_t *event;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "") != SUCCESS) {
		return;
	}

	event = emalloc(sizeof(php_event_t));
	event->event = ecalloc(1, sizeof(struct event));
	event->rsrc_id = NULL;
	event->callback = NULL;
	event->base = NULL;
	ZVAL_NULL(&event->stream_id);
	TSRMLS_SET_CTX(event->thread_ctx);

	event->rsrc_id = zend_list_insert(event, le_event);
	ZVAL_COPY_VALUE(return_value, event->rsrc_id);
}
/* }}} */

/* {{{ proto void event_free(resource event) 
 */
static PHP_FUNCTION(event_free)
{
	zval *zevent;
	php_event_t *event;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &zevent) != SUCCESS) {
		return;
	}

	if (!(event = ZVAL_TO_EVENT(zevent)))
		return;

	if (event->base) {
		--event->base->events;
		if (event->base->rsrc_id) {
			zend_list_delete(Z_RES_P(event->base->rsrc_id));
		}
		event->base = NULL;
	}

	event_del (event->event);
	zend_list_delete(Z_RES_P(event->rsrc_id));

}
/* }}} */

/* {{{ proto bool event_add(resource event[, int timeout])
 */
static PHP_FUNCTION(event_add)
{
	zval *zevent;
	php_event_t *event;
	int ret;
	zend_long timeout = -1;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r|l", &zevent, &timeout) != SUCCESS) {
		return;
	}

	if (!(event = ZVAL_TO_EVENT(zevent)))
		RETURN_FALSE;

	if (!event->base) {
		php_error_docref(NULL, E_WARNING, "Unable to add event without an event base");
		RETURN_FALSE;
	}

	if (timeout < 0) {
		ret = event_add(event->event, NULL);
	} else {
		struct timeval time;
		
		time.tv_usec = timeout % 1000000;
		time.tv_sec = timeout / 1000000;
		ret = event_add(event->event, &time);
	}

	if (ret != 0) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool event_set(resource event, mixed fd, int events, mixed callback[, mixed arg]) 
 */
static PHP_FUNCTION(event_set)
{
	zval *zevent, *fd, *zcallback, *zarg = NULL;
	php_event_t *event;
	zend_long events = 0;
	php_event_callback_t *callback, *old_callback;
	zend_string *func_name;
	php_stream *stream = NULL;
	php_socket_t file_desc;
#ifdef LIBEVENT_SOCKETS_SUPPORT
	php_socket *php_sock;
#endif
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rzlz|z", &zevent, &fd, &events, &zcallback, &zarg) != SUCCESS) {
		return;
	}

	if (!(event = ZVAL_TO_EVENT(zevent)))
		RETURN_FALSE;

	if (events & EV_TIMEOUT) {
		file_desc = -1;
		fd = 0;
		events = 0;
	} else if (events & EV_SIGNAL) {
		/* signal support */
		convert_to_long_ex(fd);
		file_desc = Z_LVAL_P(fd);
		if (file_desc < 0 || file_desc >= NSIG) {
			php_error_docref(NULL, E_WARNING, "invalid signal passed");
			RETURN_FALSE;
		}
	} else {

		if (Z_TYPE_P(fd) == IS_LONG) {
			fd = zend_hash_index_find(&EG(regular_list), Z_RES_P(fd));
			if (!fd) {
				php_error_docref(NULL, E_WARNING, "invalid file descriptor passed");
				RETURN_FALSE;
			}
		}

		if (Z_TYPE_P(fd) == IS_RESOURCE) {
			stream = zend_fetch_resource2_ex(fd, NULL, php_file_le_stream(), php_file_le_pstream());
			if (stream) {
				if (php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL, (void*)&file_desc, 1) != SUCCESS || file_desc < 0) {
					RETURN_FALSE;
				}
			} else {
#ifdef LIBEVENT_SOCKETS_SUPPORT
				php_sock = (php_socket *)zend_fetch_resource_ex(fd, NULL, php_sockets_le_socket());
				if (php_sock) {
					file_desc = php_sock->bsd_socket;
				} else {
					php_error_docref(NULL, E_WARNING, "fd argument must be either valid PHP stream or valid PHP socket resource");
					RETURN_FALSE;
				}
#else
				php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream resource");
				RETURN_FALSE;
#endif
			}
		} else {
#ifdef LIBEVENT_SOCKETS_SUPPORT
			php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream or socket resource or a file descriptor of type long");
#else
			php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream resource or a file descriptor of type long");
#endif
			RETURN_FALSE;
		}
	}

	if (!zend_is_callable(zcallback, 0, &func_name)) {
		php_error_docref(NULL, E_WARNING, "'%s' is not a valid callback", ZSTR_VAL(func_name));
		zend_string_release(func_name);
		RETURN_FALSE;
	}
	zend_string_release(func_name);

	callback = emalloc(sizeof(php_event_callback_t));
	ZVAL_COPY(&callback->func, zcallback);
	if(zarg) {
		ZVAL_COPY(&callback->arg, zarg);
	} else {
		ZVAL_NULL(&callback->arg);
	}

	old_callback = event->callback;
	event->callback = callback;

	if (!fd) {
		if (Z_TYPE_P(&event->stream_id) != IS_NULL) {
			zend_list_close(Z_RES_P(&event->stream_id));
			ZVAL_NULL(&event->stream_id);
		}
        } else if (events & EV_SIGNAL) {
		ZVAL_NULL(&event->stream_id);
	} else {
		ZVAL_COPY(&event->stream_id, fd);
	}

	event_set(event->event, (int)file_desc, (short)events, _php_event_callback, event);

	if (old_callback) {
		_php_event_callback_free(old_callback);
	}

	if (fd && event->base) {
		ret = event_base_set(event->base->base, event->event);
		if (ret != 0) {
			RETURN_FALSE;
		}
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool event_del(resource event) 
 */
static PHP_FUNCTION(event_del)
{
	zval *zevent;
	php_event_t *event;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &zevent) != SUCCESS) {
		return;
	}

	if (!(event = ZVAL_TO_EVENT(zevent)))
		RETURN_FALSE;

	if (!event->base) {
		php_error_docref(NULL, E_WARNING, "Unable to delete event without an event base");
		RETURN_FALSE;
	}

	if (event_del(event->event) == 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool event_priority_set(resource event, int priority) 
 */
static PHP_FUNCTION(event_priority_set)
{
	zval *zevent;
	php_event_t *event;
	zend_long priority = 0;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rl", &zevent, &priority) != SUCCESS) {
		return;
	}

	if (!(event = ZVAL_TO_EVENT(zevent)))
		RETURN_FALSE;

	if (!event->base) {
		php_error_docref(NULL, E_WARNING, "Unable to set event priority without an event base");
		RETURN_FALSE;
	}

	ret = event_priority_set(event->event, priority);

	if (ret == 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool event_timer_set(resource event, mixed callback[, mixed arg]) 
 */
static PHP_FUNCTION(event_timer_set)
{
	zval *zevent, *zcallback, *zarg = NULL;
	php_event_t *event;
	php_event_callback_t *callback, *old_callback;
	zend_string *func_name;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rz|z", &zevent, &zcallback, &zarg) != SUCCESS) {
		return;
	}

	if (!(event = ZVAL_TO_EVENT(zevent)))
		RETURN_FALSE;

	if (!zend_is_callable(zcallback, 0, &func_name)) {
		php_error_docref(NULL, E_WARNING, "'%s' is not a valid callback", ZSTR_VAL(func_name));
		zend_string_release(func_name);
		RETURN_FALSE;
	}
	zend_string_release(func_name);

	callback = emalloc(sizeof(php_event_callback_t));
	ZVAL_COPY(&callback->func, zcallback);
	if (zarg) {
		ZVAL_COPY(&callback->arg, zarg);
	}
	else {
		ZVAL_NULL(&callback->arg);
	}

	old_callback = event->callback;
	event->callback = callback;
	if (Z_TYPE_P(&event->stream_id) != IS_NULL) {
		zend_list_close(Z_RES_P(&event->stream_id));
		ZVAL_NULL(&event->stream_id);
	}

	event_set(event->event, -1, 0, _php_event_callback, event);

	if (old_callback) {
		_php_event_callback_free(old_callback);
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool event_timer_pending(resource event[, int timeout])
 */
static PHP_FUNCTION(event_timer_pending)
{
	zval *zevent;
	php_event_t *event;
	int ret;
	zend_long timeout = -1;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r|l", &zevent, &timeout) != SUCCESS) {
		return;
	}

	if (!(event = ZVAL_TO_EVENT(zevent)))
		RETURN_FALSE;

	if (timeout < 0) {
		ret = event_pending(event->event, EV_TIMEOUT, NULL);
	} else {
		struct timeval time;
		
		time.tv_usec = timeout % 1000000;
		time.tv_sec = timeout / 1000000;
		ret = event_pending(event->event, EV_TIMEOUT, &time);
	}

	if (ret != 0) {
		RETURN_FALSE;
	}
	RETURN_TRUE;
}
/* }}} */



/* {{{ proto resource event_buffer_new(mixed fd, mixed readcb, mixed writecb, mixed errorcb[, mixed arg]) 
 */
static PHP_FUNCTION(event_buffer_new)
{
	php_bufferevent_t *bevent;
	php_stream *stream;
	zval *zfd, *zreadcb, *zwritecb, *zerrorcb, *zarg = NULL;
	php_socket_t fd;
	zend_string *func_name;
#ifdef LIBEVENT_SOCKETS_SUPPORT
	php_socket *php_sock;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "zzzz|z", &zfd, &zreadcb, &zwritecb, &zerrorcb, &zarg) != SUCCESS) {
		return;
	}

	if (Z_TYPE_P(zfd) == IS_LONG) {
		zfd = zend_hash_index_find(&EG(regular_list), Z_RES_P(zfd));
		if (!zfd) {
			php_error_docref(NULL, E_WARNING, "invalid file descriptor passed");
			RETURN_FALSE;
		}
	}
	
	if (Z_TYPE_P(zfd) == IS_RESOURCE) {
		stream = zend_fetch_resource2_ex(zfd, NULL, php_file_le_stream(), php_file_le_pstream());
		if (stream) {
			if (php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL, (void*)&fd, 1) != SUCCESS || fd < 0) {
				RETURN_FALSE;
			}
		} else {
#ifdef LIBEVENT_SOCKETS_SUPPORT
			php_sock = (php_socket *)zend_fetch_resource_ex(zfd, NULL, php_sockets_le_socket());
			if (php_sock) {
				fd = php_sock->bsd_socket;
			} else {
				php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream or socket resource or a file descriptor of type long");
				RETURN_FALSE;
			}
#else
			php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream resource or a file descriptor of type long");
			RETURN_FALSE;
#endif
		}
	} else {
#ifdef LIBEVENT_SOCKETS_SUPPORT
		php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream or socket resource or a file descriptor of type long");
#else
		php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream resource or a file descriptor of type long");
#endif
		RETURN_FALSE;
	}

	if (Z_TYPE_P(zreadcb) != IS_NULL) {
		if (!zend_is_callable(zreadcb, 0, &func_name)) {
			php_error_docref(NULL, E_WARNING, "'%s' is not a valid read callback", ZSTR_VAL(func_name));
			zend_string_release(func_name);
			RETURN_FALSE;
		}
		zend_string_release(func_name);
	} else {
		zreadcb = NULL;
	}

	if (Z_TYPE_P(zwritecb) != IS_NULL) {
		if (!zend_is_callable(zwritecb, 0, &func_name)) {
			php_error_docref(NULL, E_WARNING, "'%s' is not a valid write callback", ZSTR_VAL(func_name));
			zend_string_release(func_name);
			RETURN_FALSE;
		}
		zend_string_release(func_name);
	} else {
		zwritecb = NULL;
	}

	if (!zend_is_callable(zerrorcb, 0, &func_name)) {
		php_error_docref(NULL, E_WARNING, "'%s' is not a valid error callback", ZSTR_VAL(func_name));
		zend_string_release(func_name);
		RETURN_FALSE;
	}
	zend_string_release(func_name);

	bevent = emalloc(sizeof(php_bufferevent_t));
	bevent->bevent = bufferevent_new(fd, _php_bufferevent_readcb, _php_bufferevent_writecb, _php_bufferevent_errorcb, bevent);
	bevent->rsrc_id = NULL;
	bevent->base = NULL;

	if (zreadcb) {
		ZVAL_COPY(&bevent->readcb, zreadcb);
	}

	if (zwritecb) {
		ZVAL_COPY(&bevent->writecb, zwritecb);
	}
		
	if (zerrorcb) {
		ZVAL_COPY(&bevent->errorcb, zerrorcb);
	}

	if (zarg) {
		ZVAL_COPY(&bevent->arg, zarg);
	}
	else {
		ZVAL_NULL(&bevent->arg);
	}

	TSRMLS_SET_CTX(bevent->thread_ctx);

	bevent->rsrc_id = zend_list_insert(bevent, le_bufferevent);
	ZVAL_COPY_VALUE(return_value, bevent->rsrc_id);
}
/* }}} */

/* {{{ proto void event_buffer_free(resource bevent) 
 */
static PHP_FUNCTION(event_buffer_free)
{
	zval *zbevent;
	php_bufferevent_t *bevent;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &zbevent) != SUCCESS) {
		return;
	}

	if (!(bevent = ZVAL_TO_BEVENT(zbevent)))
		return;
	
	if (bevent->base) {
		--bevent->base->events;
		if (bevent->base->rsrc_id) {
			zend_list_delete(Z_RES_P(bevent->base->rsrc_id));
		}
		bevent->base = NULL;
	}
	zend_list_close(Z_RES_P(bevent->rsrc_id));
}
/* }}} */

/* {{{ proto bool event_buffer_base_set(resource bevent, resource base) 
 */
static PHP_FUNCTION(event_buffer_base_set)
{
	zval *zbase, *zbevent;
	php_event_base_t *base, *old_base;
	php_bufferevent_t *bevent;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rr", &zbevent, &zbase) != SUCCESS) {
		return;
	}

	if (!(base = ZVAL_TO_BASE(zbase)))
		RETURN_FALSE;
	if (!(bevent = ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;

	old_base = bevent->base;
	ret = bufferevent_base_set(base->base, bevent->bevent);

	if (ret == 0) {
		if (base != old_base) {
			/* make sure the base is destroyed after the event */
			Z_ADDREF_P(base->rsrc_id);
			++base->events;

			/* deference the event from the old base */
			if (old_base) {
				--old_base->events;
				if (old_base->rsrc_id) {
					zend_list_delete(Z_RES_P(old_base->rsrc_id));
				}
			}
		}

		bevent->base = base;
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool event_buffer_priority_set(resource bevent, int priority) 
 */
static PHP_FUNCTION(event_buffer_priority_set)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	zend_long priority = 0;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rl", &zbevent, &priority) != SUCCESS) {
		return;
	}

	if (!(bevent = ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;

	if (!bevent->base) {
		php_error_docref(NULL, E_WARNING, "Unable to set event priority without an event base");
		RETURN_FALSE;
	}

	ret = bufferevent_priority_set(bevent->bevent, priority);

	if (ret == 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool event_buffer_write(resource bevent, string data[, int data_size]) 
 */
static PHP_FUNCTION(event_buffer_write)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	char *data;
	size_t data_len;
	zend_long data_size = -1;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rs|l", &zbevent, &data, &data_len, &data_size) != SUCCESS) {
		return;
	}

	if (!(bevent = ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;

	if (ZEND_NUM_ARGS() < 3 || data_size < 0) {
		data_size = data_len;
	} else if (data_size > data_len) {
		php_error_docref(NULL, E_WARNING, "data_size out of range");
		RETURN_FALSE;
	}

	ret = bufferevent_write(bevent->bevent, (const void *)data, data_size);

	if (ret == 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto string event_buffer_read(resource bevent, int data_size) 
 */
static PHP_FUNCTION(event_buffer_read)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	char *data;
	zend_long data_size = 0;
	zend_string *str = NULL;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rl", &zbevent, &data_size) != SUCCESS) {
		return;
	}

	if (!(bevent= ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;

	if (data_size == 0) {
		RETURN_EMPTY_STRING();
	} else if (data_size < 0) {
		php_error_docref(NULL, E_WARNING, "data_size cannot be less than zero");
		RETURN_FALSE;
	}

	data = safe_emalloc((int)data_size, sizeof(char), 1);
	ret = bufferevent_read(bevent->bevent, data, data_size);

	if (ret > 0) {
		if (ret > data_size) { /* paranoia */
			ret = data_size;
		}
		data[ret] = '\0';
	}
	str = zend_string_init(data, ret, 0);
	safe_efree(data);
	RETURN_STR(str);
}
/* }}} */

/* {{{ proto bool event_buffer_enable(resource bevent, int events) 
 */
static PHP_FUNCTION(event_buffer_enable)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	zend_long events = 0;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rl", &zbevent, &events) != SUCCESS) {
		return;
	}

	if (!(bevent = ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;

	ret = bufferevent_enable(bevent->bevent, events);

	if (ret == 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool event_buffer_disable(resource bevent, int events) 
 */
static PHP_FUNCTION(event_buffer_disable)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	zend_long events = 0;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rl", &zbevent, &events) != SUCCESS) {
		return;
	}

	if (!(bevent = ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;

	if (bevent) {
		ret = bufferevent_disable(bevent->bevent, events);

		if (ret == 0) {
			RETURN_TRUE;
		}
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto void event_buffer_timeout_set(resource bevent, int read_timeout, int write_timeout) 
 */
static PHP_FUNCTION(event_buffer_timeout_set)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	zend_long read_timeout, write_timeout;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rll", &zbevent, &read_timeout, &write_timeout) != SUCCESS) {
		return;
	}

	if (!(bevent = ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;
	bufferevent_settimeout(bevent->bevent, read_timeout, write_timeout);
}
/* }}} */

/* {{{ proto void event_buffer_watermark_set(resource bevent, int events, int lowmark, int highmark) 
 */
static PHP_FUNCTION(event_buffer_watermark_set)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	zend_long events, lowmark, highmark;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rlll", &zbevent, &events, &lowmark, &highmark) != SUCCESS) {
		return;
	}

	if (!(bevent = ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;
	bufferevent_setwatermark(bevent->bevent, events, lowmark, highmark);
}
/* }}} */

/* {{{ proto void event_buffer_fd_set(resource bevent, resource fd) 
 */
static PHP_FUNCTION(event_buffer_fd_set)
{
	zval *zbevent, *zfd;
	php_stream *stream;
	php_bufferevent_t *bevent;
	php_socket_t fd;
#ifdef LIBEVENT_SOCKETS_SUPPORT
	php_socket *php_sock;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rz", &zbevent, &zfd) != SUCCESS) {
		return;
	}

	if(!(bevent= ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;

	if (Z_TYPE_P(zfd) == IS_LONG) {
		zfd = zend_hash_index_find(&EG(regular_list), Z_RES_P(zfd));
		if (!zfd) {
			php_error_docref(NULL, E_WARNING, "invalid file descriptor passed");
			RETURN_FALSE;
		}
	}

	if (Z_TYPE_P(zfd) == IS_RESOURCE) {
		stream = zend_fetch_resource2_ex(zfd, NULL, php_file_le_stream(), php_file_le_pstream());
		if (stream) {
			if (php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL, (void*)&fd, 1) != SUCCESS || fd < 0) {
				RETURN_FALSE;
			}
		} else {
#ifdef LIBEVENT_SOCKETS_SUPPORT
			php_sock = (php_socket *)zend_fetch_resource_ex(zfd, NULL, php_sockets_le_socket());
			if (php_sock) {
				fd = php_sock->bsd_socket;
			} else {
				php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream or socket resource or a file descriptor of type long");
				RETURN_FALSE;
			}
#else
			php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream resource or a file descriptor of type long");
			RETURN_FALSE;
#endif
		}
	} else {
#ifdef LIBEVENT_SOCKETS_SUPPORT
		php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream or socket resource or a file descriptor of type long");
#else
		php_error_docref(NULL, E_WARNING, "fd argument must be valid PHP stream resource or a file descriptor of type long");
#endif
		RETURN_FALSE;
	}

	bufferevent_setfd(bevent->bevent, fd);
}
/* }}} */

/* {{{ proto resource event_buffer_set_callback(resource bevent, mixed readcb, mixed writecb, mixed errorcb[, mixed arg]) 
 */
static PHP_FUNCTION(event_buffer_set_callback)
{
	php_bufferevent_t *bevent;
	zval *zbevent, *zreadcb, *zwritecb, *zerrorcb, *zarg = NULL;
	zend_string *func_name;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rzzz|z", &zbevent, &zreadcb, &zwritecb, &zerrorcb, &zarg) != SUCCESS) {
		return;
	}

	if (!(bevent= ZVAL_TO_BEVENT(zbevent)))
		RETURN_FALSE;

	if (Z_TYPE_P(zreadcb) != IS_NULL) {
		if (!zend_is_callable(zreadcb, 0, &func_name)) {
			php_error_docref(NULL, E_WARNING, "'%s' is not a valid read callback", ZSTR_VAL(func_name));
			zend_string_release(func_name);
			RETURN_FALSE;
		}
		zend_string_release(func_name);
	} else {
		zreadcb = NULL;
	}

	if (Z_TYPE_P(zwritecb) != IS_NULL) {
		if (!zend_is_callable(zwritecb, 0, &func_name)) {
			php_error_docref(NULL, E_WARNING, "'%s' is not a valid write callback", ZSTR_VAL(func_name));
			zend_string_release(func_name);
			RETURN_FALSE;
		}
		zend_string_release(func_name);
	} else {
		zwritecb = NULL;
	}

	if (Z_TYPE_P(zerrorcb) != IS_NULL) {
		if (!zend_is_callable(zerrorcb, 0, &func_name)) {
			php_error_docref(NULL, E_WARNING, "'%s' is not a valid error callback", ZSTR_VAL(func_name));
			zend_string_release(func_name);
			RETURN_FALSE;
		}
		zend_string_release(func_name);
	} else {
		zerrorcb = NULL;
	}

	if (!Z_ISUNDEF(bevent->readcb)) {
		zval_ptr_dtor(&bevent->readcb);
	}
	if (!Z_ISUNDEF(bevent->writecb)) {
		zval_ptr_dtor(&bevent->writecb);
	}
	if (!Z_ISUNDEF(bevent->errorcb)) {
		zval_ptr_dtor(&bevent->errorcb);
	}
	if (!Z_ISUNDEF(bevent->arg)) {
		zval_ptr_dtor(&bevent->arg);
	}

	if (zreadcb) {
		ZVAL_COPY(&bevent->readcb, zreadcb);
	} else {
		ZVAL_UNDEF(&bevent->readcb);
	}

	if (zwritecb) {
		ZVAL_COPY(&bevent->writecb, zwritecb);
	} else {
		ZVAL_UNDEF(&bevent->writecb);
	}
	
	if (zerrorcb) {
		ZVAL_COPY(&bevent->errorcb, zerrorcb);
	} else {
		ZVAL_UNDEF(&bevent->errorcb);
	}
	
	if (zarg) {
		ZVAL_COPY(&bevent->arg, zarg);
	} else {
		ZVAL_NULL(&bevent->arg);
	}

	RETURN_TRUE;
}
/* }}} */


/* {{{ PHP_MINIT_FUNCTION
 */
static PHP_MINIT_FUNCTION(libevent)
{
	le_event_base = zend_register_list_destructors_ex(_php_event_base_dtor, NULL, "event base", module_number);
	le_event = zend_register_list_destructors_ex(_php_event_dtor, NULL, "event", module_number);
	le_bufferevent = zend_register_list_destructors_ex(_php_bufferevent_dtor, NULL, "buffer event", module_number);

	REGISTER_LONG_CONSTANT("EV_TIMEOUT", EV_TIMEOUT, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EV_READ", EV_READ, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EV_WRITE", EV_WRITE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EV_SIGNAL", EV_SIGNAL, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EV_PERSIST", EV_PERSIST, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EVLOOP_NONBLOCK", EVLOOP_NONBLOCK, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EVLOOP_ONCE", EVLOOP_ONCE, CONST_CS | CONST_PERSISTENT);
	
	REGISTER_LONG_CONSTANT("EVBUFFER_READ", EVBUFFER_READ, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EVBUFFER_WRITE", EVBUFFER_WRITE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EVBUFFER_EOF", EVBUFFER_EOF, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EVBUFFER_ERROR", EVBUFFER_ERROR, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("EVBUFFER_TIMEOUT", EVBUFFER_TIMEOUT, CONST_CS | CONST_PERSISTENT);
#ifdef BEV_EVENT_CONNECTED // for libevent2
	REGISTER_LONG_CONSTANT("EVBUFFER_CONNECTED", BEV_EVENT_CONNECTED, CONST_CS | CONST_PERSISTENT);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(libevent)
{
	char buf[64];


	php_info_print_table_start();
	php_info_print_table_header(2, "libevent support", "enabled");
	php_info_print_table_row(2, "extension version", PHP_LIBEVENT_VERSION);
	php_info_print_table_row(2, "Revision", "$Revision$");
	
	snprintf(buf, sizeof(buf) - 1, "%s", event_get_version());
	php_info_print_table_row(2, "libevent version", buf);

	php_info_print_table_end();
}
/* }}} */

#if PHP_MAJOR_VERSION >= 5
/* {{{ arginfo */
#if (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3) || PHP_MAJOR_VERSION > 5
# define EVENT_ARGINFO
#else
# define EVENT_ARGINFO static
#endif

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_base_loop, 0, 0, 1)
	ZEND_ARG_INFO(0, base)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_base_loopbreak, 0, 0, 1)
	ZEND_ARG_INFO(0, base)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_base_loopexit, 0, 0, 1)
	ZEND_ARG_INFO(0, base)
	ZEND_ARG_INFO(0, timeout)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_base_set, 0, 0, 2)
	ZEND_ARG_INFO(0, event)
	ZEND_ARG_INFO(0, base)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_base_priority_init, 0, 0, 2)
	ZEND_ARG_INFO(0, base)
	ZEND_ARG_INFO(0, npriorities)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO(arginfo_event_new, 0)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_add, 0, 0, 1)
	ZEND_ARG_INFO(0, event)
	ZEND_ARG_INFO(0, timeout)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_set, 0, 0, 4)
	ZEND_ARG_INFO(0, event)
	ZEND_ARG_INFO(0, fd)
	ZEND_ARG_INFO(0, events)
	ZEND_ARG_INFO(0, callback)
	ZEND_ARG_INFO(0, arg)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_del, 0, 0, 1)
	ZEND_ARG_INFO(0, event)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_priority_set, 0, 0, 2)
	ZEND_ARG_INFO(0, event)
	ZEND_ARG_INFO(0, priority)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_new, 0, 0, 4)
	ZEND_ARG_INFO(0, stream)
	ZEND_ARG_INFO(0, readcb)
	ZEND_ARG_INFO(0, writecb)
	ZEND_ARG_INFO(0, errorcb)
	ZEND_ARG_INFO(0, arg)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_free, 0, 0, 1)
	ZEND_ARG_INFO(0, bevent)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_base_set, 0, 0, 2)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, base)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_priority_set, 0, 0, 2)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, priority)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_write, 0, 0, 2)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, data)
	ZEND_ARG_INFO(0, data_size)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_read, 0, 0, 2)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, data_size)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_disable, 0, 0, 2)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, events)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_timeout_set, 0, 0, 3)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, read_timeout)
	ZEND_ARG_INFO(0, write_timeout)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_watermark_set, 0, 0, 4)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, events)
	ZEND_ARG_INFO(0, lowmark)
	ZEND_ARG_INFO(0, highmark)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_fd_set, 0, 0, 2)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, fd)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_buffer_set_callback, 0, 0, 4)
	ZEND_ARG_INFO(0, bevent)
	ZEND_ARG_INFO(0, readcb)
	ZEND_ARG_INFO(0, writecb)
	ZEND_ARG_INFO(0, errorcb)
	ZEND_ARG_INFO(0, arg)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_timer_set, 0, 0, 2)
	ZEND_ARG_INFO(0, event)
	ZEND_ARG_INFO(0, callback)
	ZEND_ARG_INFO(0, arg)
ZEND_END_ARG_INFO()

EVENT_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_event_timer_pending, 0, 0, 1)
	ZEND_ARG_INFO(0, event)
	ZEND_ARG_INFO(0, timeout)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ libevent_functions[]
 */
#if ZEND_MODULE_API_NO >= 20071006
const 
#endif
zend_function_entry libevent_functions[] = {
	PHP_FE(event_base_new, 				arginfo_event_new)
	PHP_FE(event_base_reinit, 			arginfo_event_base_loopbreak)
	PHP_FE(event_base_free, 			arginfo_event_base_loopbreak)
	PHP_FE(event_base_loop, 			arginfo_event_base_loop)
	PHP_FE(event_base_loopbreak, 		arginfo_event_base_loopbreak)
	PHP_FE(event_base_loopexit, 		arginfo_event_base_loopexit)
	PHP_FE(event_base_set, 				arginfo_event_base_set)
	PHP_FE(event_base_priority_init, 	arginfo_event_base_priority_init)
	PHP_FE(event_new, 					arginfo_event_new)
	PHP_FE(event_free, 					arginfo_event_del)
	PHP_FE(event_add, 					arginfo_event_add)
	PHP_FE(event_set, 					arginfo_event_set)
	PHP_FE(event_del, 					arginfo_event_del)
	PHP_FE(event_priority_set, 			arginfo_event_priority_set)
	PHP_FE(event_buffer_new, 			arginfo_event_buffer_new)
	PHP_FE(event_buffer_free, 			arginfo_event_buffer_free)
	PHP_FE(event_buffer_base_set, 		arginfo_event_buffer_base_set)
	PHP_FE(event_buffer_priority_set, 	arginfo_event_buffer_priority_set)
	PHP_FE(event_buffer_write, 			arginfo_event_buffer_write)
	PHP_FE(event_buffer_read, 			arginfo_event_buffer_read)
	PHP_FE(event_buffer_enable, 		arginfo_event_buffer_disable)
	PHP_FE(event_buffer_disable, 		arginfo_event_buffer_disable)
	PHP_FE(event_buffer_timeout_set, 	arginfo_event_buffer_timeout_set)
	PHP_FE(event_buffer_watermark_set, 	arginfo_event_buffer_watermark_set)
	PHP_FE(event_buffer_fd_set, 		arginfo_event_buffer_fd_set)
	PHP_FE(event_buffer_set_callback, 	arginfo_event_buffer_set_callback)
	PHP_FALIAS(event_timer_new,			event_new,		arginfo_event_new)
	PHP_FE(event_timer_set,				arginfo_event_timer_set)
	PHP_FE(event_timer_pending,			arginfo_event_timer_pending)
	PHP_FALIAS(event_timer_add,			event_add,		arginfo_event_add)
	PHP_FALIAS(event_timer_del,			event_del,		arginfo_event_del)
	{NULL, NULL, NULL}
};
/* }}} */
#else
/* {{{ libevent_functions[]
 */
zend_function_entry libevent_functions[] = {
	PHP_FE(event_base_new, 				NULL)
	PHP_FE(event_base_reinit, 			NULL)
	PHP_FE(event_base_free, 			NULL)
	PHP_FE(event_base_loop, 			NULL)
	PHP_FE(event_base_loopbreak, 		NULL)
	PHP_FE(event_base_loopexit, 		NULL)
	PHP_FE(event_base_set, 				NULL)
	PHP_FE(event_base_priority_init,	NULL)
	PHP_FE(event_new, 					NULL)
	PHP_FE(event_free, 					NULL)
	PHP_FE(event_add, 					NULL)
	PHP_FE(event_set, 					NULL)
	PHP_FE(event_del, 					NULL)
	PHP_FE(event_priority_set, 			NULL)
	PHP_FE(event_buffer_new, 			NULL)
	PHP_FE(event_buffer_free, 			NULL)
	PHP_FE(event_buffer_base_set, 		NULL)
	PHP_FE(event_buffer_priority_set, 	NULL)
	PHP_FE(event_buffer_write, 			NULL)
	PHP_FE(event_buffer_read, 			NULL)
	PHP_FE(event_buffer_enable, 		NULL)
	PHP_FE(event_buffer_disable, 		NULL)
	PHP_FE(event_buffer_timeout_set, 	NULL)
	PHP_FE(event_buffer_watermark_set, 	NULL)
	PHP_FE(event_buffer_fd_set, 		NULL)
	PHP_FALIAS(event_timer_new,			event_new,	NULL)
	PHP_FE(event_timer_set,				NULL)
	PHP_FE(event_timer_pending,			NULL)
	PHP_FALIAS(event_timer_add,			event_add,	NULL)
	PHP_FALIAS(event_timer_del,			event_del,	NULL)
	{NULL, NULL, NULL}
};
/* }}} */
#endif

static const zend_module_dep libevent_deps[] = { /* {{{ */
	ZEND_MOD_OPTIONAL("sockets")
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ libevent_module_entry
 */
zend_module_entry libevent_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	libevent_deps,
	"libevent",
	libevent_functions,
	PHP_MINIT(libevent),
	NULL,
	NULL,
	NULL,
	PHP_MINFO(libevent),
	PHP_LIBEVENT_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
