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

#include <event.h>

static int le_event_base;
static int le_event;
static int le_bufferevent;

#ifdef COMPILE_DL_LIBEVENT
ZEND_GET_MODULE(libevent)
#endif

typedef struct _php_event_base_t { /* {{{ */
	struct event_base *base;
	int rsrc_id;
} php_event_base_t;
/* }}} */

typedef struct _php_event_callback_t { /* {{{ */
	zval *func;
	zval *arg;
} php_event_callback_t;
/* }}} */

typedef struct _php_event_t { /* {{{ */
	struct event *event;
	int rsrc_id;
	int stream_id;
	php_event_base_t *base;
	php_event_callback_t *callback;
} php_event_t;
/* }}} */

typedef struct _php_bufferevent_t { /* {{{ */
	struct bufferevent *bevent;
	int rsrc_id;
	php_event_base_t *base;
	zval *readcb;
	zval *writecb;
	zval *errorcb;
	zval *arg;
} php_bufferevent_t;
/* }}} */

#define ZVAL_TO_BASE(zval, base) \
	ZEND_FETCH_RESOURCE(base, php_event_base_t *, &zval, -1, "event base", le_event_base)

#define ZVAL_TO_EVENT(zval, event) \
	ZEND_FETCH_RESOURCE(event, php_event_t *, &zval, -1, "event", le_event)

#define ZVAL_TO_BEVENT(zval, bevent) \
	ZEND_FETCH_RESOURCE(bevent, php_bufferevent_t *, &zval, -1, "buffer event", le_bufferevent)

/* {{{ internal funcs */

static inline void _php_event_callback_free(php_event_callback_t *callback) /* {{{ */
{
	if (!callback) {
		return;
	}

	zval_ptr_dtor(&callback->func);
	if (callback->arg) {
		zval_ptr_dtor(&callback->arg);
	}
	efree(callback);
}
/* }}} */

static void _php_event_base_dtor(zend_rsrc_list_entry *rsrc TSRMLS_DC) /* {{{ */
{
	php_event_base_t *base = (php_event_base_t*)rsrc->ptr;

	event_base_free(base->base);
	efree(base);
}
/* }}} */

static void _php_event_dtor(zend_rsrc_list_entry *rsrc TSRMLS_DC) /* {{{ */
{
	php_event_t *event = (php_event_t*)rsrc->ptr;
	int base_id = -1;

	if (event->base) {
		base_id = event->base->rsrc_id;
	}
	if (event->stream_id >= 0) {
		zend_list_delete(event->stream_id);
	}
	event_del(event->event);

	_php_event_callback_free(event->callback);
	efree(event->event);
	efree(event);

	if (base_id >= 0) {
		zend_list_delete(base_id);
	}
}
/* }}} */

static void _php_bufferevent_dtor(zend_rsrc_list_entry *rsrc TSRMLS_DC) /* {{{ */
{
	php_bufferevent_t *bevent = (php_bufferevent_t*)rsrc->ptr;
	int base_id = -1;

	if (bevent->base) {
		base_id = bevent->base->rsrc_id;
	}
	if (bevent->readcb) {
		zval_ptr_dtor(&(bevent->readcb));
	}
	if (bevent->writecb) {
		zval_ptr_dtor(&(bevent->writecb));
	}
	if (bevent->errorcb) {
		zval_ptr_dtor(&(bevent->errorcb));
	}
	if (bevent->arg) {
		zval_ptr_dtor(&(bevent->arg));
	}

	bufferevent_free(bevent->bevent);
	efree(bevent);

	if (base_id >= 0) {
		zend_list_delete(base_id);
	}
}
/* }}} */

static void _php_event_callback(int fd, short events, void *arg) /* {{{ */
{
	zval *args[3];
	php_event_t *event = (php_event_t *)arg;
	php_event_callback_t *callback;
	zval retval;
	int base_id;
	TSRMLS_FETCH();

	if (!event || !event->callback || !event->base) {
		return;
	}

	base_id = event->base->rsrc_id;
	zend_list_addref(event->rsrc_id);
	zend_list_addref(event->base->rsrc_id);

	callback = event->callback;

	MAKE_STD_ZVAL(args[0]);
	ZVAL_RESOURCE(args[0], event->stream_id);
	zend_list_addref(event->stream_id);
	
	MAKE_STD_ZVAL(args[1]);
	ZVAL_LONG(args[1], events);

	if (callback->arg) {
		args[2] = callback->arg;
	} else {
		MAKE_STD_ZVAL(args[2]);
		ZVAL_NULL(args[2]);
	}
	
	if (call_user_function(EG(function_table), NULL, callback->func, &retval, 3, args TSRMLS_CC) == SUCCESS) {
		zval_dtor(&retval);
	}

	zval_ptr_dtor(&(args[0]));
	zval_ptr_dtor(&(args[1]));
	if (!callback->arg) {
		zval_ptr_dtor(&(args[2])); 
	} 
	
	zend_list_delete(event->rsrc_id); /* event and its contents is undefined after this */
	zend_list_delete(base_id);
}
/* }}} */

static void _php_bufferevent_readcb(struct bufferevent *be, void *arg) /* {{{ */
{
	zval *args[2];
	zval retval;
	int base_id;
	php_bufferevent_t *bevent = (php_bufferevent_t *)arg;
	TSRMLS_FETCH();

	if (!bevent || !bevent->base || !bevent->readcb) {
		return;
	}

	base_id = bevent->base->rsrc_id;
	zend_list_addref(bevent->rsrc_id);
	zend_list_addref(bevent->base->rsrc_id);

	MAKE_STD_ZVAL(args[0]);
	ZVAL_RESOURCE(args[0], bevent->rsrc_id);
	zend_list_addref(bevent->rsrc_id); /* we do refcount-- later in zval_ptr_dtor */
	
	if (bevent->arg) {
		args[1] = bevent->arg;
	} else {
		MAKE_STD_ZVAL(args[1]);
		ZVAL_NULL(args[1]);
	}
	
	if (call_user_function(EG(function_table), NULL, bevent->readcb, &retval, 2, args TSRMLS_CC) == SUCCESS) {
		zval_dtor(&retval);
	}

	zval_ptr_dtor(&(args[0]));
	if (!bevent->arg) {
		zval_ptr_dtor(&(args[1])); 
	} 
	
	zend_list_delete(bevent->rsrc_id);
	zend_list_delete(base_id);
}
/* }}} */

static void _php_bufferevent_writecb(struct bufferevent *be, void *arg) /* {{{ */
{
	zval *args[2];
	zval retval;
	int base_id;
	php_bufferevent_t *bevent = (php_bufferevent_t *)arg;
	TSRMLS_FETCH();

	if (!bevent || !bevent->base || !bevent->writecb) {
		return;
	}

	base_id = bevent->base->rsrc_id;
	zend_list_addref(bevent->rsrc_id);
	zend_list_addref(bevent->base->rsrc_id);

	MAKE_STD_ZVAL(args[0]);
	ZVAL_RESOURCE(args[0], bevent->rsrc_id);
	zend_list_addref(bevent->rsrc_id); /* we do refcount-- later in zval_ptr_dtor */
	
	if (bevent->arg) {
		args[1] = bevent->arg;
	} else {
		MAKE_STD_ZVAL(args[1]);
		ZVAL_NULL(args[1]);
	}
	
	if (call_user_function(EG(function_table), NULL, bevent->writecb, &retval, 2, args TSRMLS_CC) == SUCCESS) {
		zval_dtor(&retval);
	}

	zval_ptr_dtor(&(args[0]));
	if (!bevent->arg) {
		zval_ptr_dtor(&(args[1])); 
	} 
	
	zend_list_delete(bevent->rsrc_id);
	zend_list_delete(base_id);
}
/* }}} */

static void _php_bufferevent_errorcb(struct bufferevent *be, short what, void *arg) /* {{{ */
{
	zval *args[3];
	zval retval;
	int base_id;
	php_bufferevent_t *bevent = (php_bufferevent_t *)arg;
	TSRMLS_FETCH();

	if (!bevent || !bevent->base || !bevent->writecb) {
		return;
	}

	base_id = bevent->base->rsrc_id;
	zend_list_addref(bevent->rsrc_id);
	zend_list_addref(bevent->base->rsrc_id);

	MAKE_STD_ZVAL(args[0]);
	ZVAL_RESOURCE(args[0], bevent->rsrc_id);
	zend_list_addref(bevent->rsrc_id); /* we do refcount-- later in zval_ptr_dtor */
	
	MAKE_STD_ZVAL(args[1]);
	ZVAL_LONG(args[1], what);

	if (bevent->arg) {
		args[2] = bevent->arg;
	} else {
		MAKE_STD_ZVAL(args[2]);
		ZVAL_NULL(args[2]);
	}
	
	if (call_user_function(EG(function_table), NULL, bevent->errorcb, &retval, 3, args TSRMLS_CC) == SUCCESS) {
		zval_dtor(&retval);
	}

	zval_ptr_dtor(&(args[0]));
	zval_ptr_dtor(&(args[1]));
	if (!bevent->arg) {
		zval_ptr_dtor(&(args[2])); 
	} 
	
	zend_list_delete(bevent->rsrc_id);
	zend_list_delete(base_id);
}
/* }}} */

/* }}} */


/* {{{ proto resource event_base_new() 
 */
static PHP_FUNCTION(event_base_new)
{
	php_event_base_t *base;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") != SUCCESS) {
		return;
	}

	base = emalloc(sizeof(php_event_base_t));
	base->base = event_base_new();
	if (!base->base) {
		efree(base);
		RETURN_FALSE;
	}

	base->rsrc_id = zend_list_insert(base, le_event_base);
	RETURN_RESOURCE(base->rsrc_id);
}
/* }}} */

/* {{{ proto void event_base_free(resource base) 
 */
static PHP_FUNCTION(event_base_free)
{
	zval *zbase;
	php_event_base_t *base;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zbase) != SUCCESS) {
		return;
	}

	ZVAL_TO_BASE(zbase, base);
	zend_list_delete(base->rsrc_id);
}
/* }}} */

/* {{{ proto int event_base_loop(resource base[, int flags]) 
 */
static PHP_FUNCTION(event_base_loop)
{
	zval *zbase;
	php_event_base_t *base;
	long flags = 0;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &zbase, &flags) != SUCCESS) {
		return;
	}

	ZVAL_TO_BASE(zbase, base);
	zend_list_addref(base->rsrc_id); /* make sure the base cannot be destroyed during the loop */
	ret = event_base_loop(base->base, flags);
	zend_list_delete(base->rsrc_id);

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

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zbase) != SUCCESS) {
		return;
	}

	ZVAL_TO_BASE(zbase, base);
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
	long timeout = -1;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &zbase, &timeout) != SUCCESS) {
		return;
	}

	ZVAL_TO_BASE(zbase, base);

	if (timeout < 0) {
		ret = event_base_loopexit(base->base, NULL);
	} else {
		struct timeval time;
		
		time.tv_usec = timeout % 1000000;
		time.tv_sec = timeout / 1000000;
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

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rr", &zevent, &zbase) != SUCCESS) {
		return;
	}

	ZVAL_TO_BASE(zbase, base);
	ZVAL_TO_EVENT(zevent, event);

	old_base = event->base;
	ret = event_base_set(base->base, event->event);

	if (ret == 0) {
		if (base != old_base) {
			/* make sure the base is destroyed after the event */
			zend_list_addref(base->rsrc_id);
		}

		if (old_base) {
			zend_list_delete(old_base->rsrc_id);
		}

		event->base = base;
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

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") != SUCCESS) {
		return;
	}

	event = emalloc(sizeof(php_event_t));
	event->event = ecalloc(1, sizeof(struct event));

	event->stream_id = -1;
	event->callback = NULL;
	event->base = NULL;

	event->rsrc_id = zend_list_insert(event, le_event);
	RETURN_RESOURCE(event->rsrc_id);
}
/* }}} */

/* {{{ proto void event_free(resource event) 
 */
static PHP_FUNCTION(event_free)
{
	zval *zevent;
	php_event_t *event;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zevent) != SUCCESS) {
		return;
	}

	ZVAL_TO_EVENT(zevent, event);
	zend_list_delete(event->rsrc_id);
}
/* }}} */

/* {{{ proto bool event_add(resource event[, int timeout])
 */
static PHP_FUNCTION(event_add)
{
	zval *zevent;
	php_event_t *event;
	int ret;
	long timeout = -1;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &zevent, &timeout) != SUCCESS) {
		return;
	}

	ZVAL_TO_EVENT(zevent, event);

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

/* {{{ proto bool event_set(resource event, resource fd, int events, mixed callback[, mixed arg]) 
 */
static PHP_FUNCTION(event_set)
{
	zval *zevent, *fd, *zcallback, *zarg = NULL;
	php_event_t *event;
	long events;
	php_event_callback_t *callback, *old_callback;
	char *func_name;
	php_stream *stream;
	php_socket_t file_desc;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rrlz|z", &zevent, &fd, &events, &zcallback, &zarg) != SUCCESS) {
		return;
	}

	ZVAL_TO_EVENT(zevent, event);
	php_stream_from_zval(stream, &fd);

	if (php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL, (void*)&file_desc, 1) != SUCCESS || file_desc < 0) {
		RETURN_FALSE;
	}

	if (!zend_is_callable(zcallback, 0, &func_name TSRMLS_CC)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "'%s' is not a invalid callback", func_name);
		efree(func_name);
		RETURN_FALSE;
	}
	efree(func_name);

	zval_add_ref(&zcallback);
	if (zarg) {
		zval_add_ref(&zarg);
	}

	callback = emalloc(sizeof(php_event_callback_t));
	callback->func = zcallback;
	callback->arg = zarg;

	old_callback = event->callback;
	event->callback = callback;
	zend_list_addref(Z_LVAL_P(fd));
	event->stream_id = Z_LVAL_P(fd);

	event_set(event->event, (int)file_desc, (short)events, _php_event_callback, event);

	if (old_callback) {
		_php_event_callback_free(old_callback);
	}
}
/* }}} */

/* {{{ proto void event_del(resource event) 
 */
static PHP_FUNCTION(event_del)
{
	zval *zevent;
	php_event_t *event;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zevent) != SUCCESS) {
		return;
	}

	ZVAL_TO_EVENT(zevent, event);
	event_del(event->event);	
}
/* }}} */


/* {{{ proto resource event_buffer_new(resource stream, mixed readcb, mixed writecb, mixed errorcb[, mixed arg]) 
 */
static PHP_FUNCTION(event_buffer_new)
{
	php_bufferevent_t *bevent;
	php_stream *stream;
	zval *zstream, *zreadcb, *zwritecb, *zerrorcb, *zarg = NULL;
	php_socket_t fd;
	char *func_name;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rzzz|z", &zstream, &zreadcb, &zwritecb, &zerrorcb, &zarg) != SUCCESS) {
		return;
	}

	php_stream_from_zval(stream, &zstream);

	if (php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL, (void*)&fd, 1) != SUCCESS || fd < 0) {
		RETURN_FALSE;
	}

	if (Z_TYPE_P(zreadcb) != IS_NULL) {
		if (!zend_is_callable(zreadcb, 0, &func_name TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "'%s' is not a invalid read callback", func_name);
			efree(func_name);
			RETURN_FALSE;
		}
		efree(func_name);
	} else {
		zreadcb = NULL;
	}

	if (Z_TYPE_P(zwritecb) != IS_NULL) {
		if (!zend_is_callable(zwritecb, 0, &func_name TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "'%s' is not a invalid write callback", func_name);
			efree(func_name);
			RETURN_FALSE;
		}
		efree(func_name);
	} else {
		zwritecb = NULL;
	}

	if (!zend_is_callable(zerrorcb, 0, &func_name TSRMLS_CC)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "'%s' is not a invalid error callback", func_name);
		efree(func_name);
		RETURN_FALSE;
	}
	efree(func_name);

	bevent = emalloc(sizeof(php_bufferevent_t));
	bevent->bevent = bufferevent_new(fd, _php_bufferevent_readcb, _php_bufferevent_writecb, _php_bufferevent_errorcb, bevent);

	bevent->base = NULL;

	if (zreadcb) {
		zval_add_ref(&zreadcb);
	}
	bevent->readcb = zreadcb;
	
	if (zwritecb) {
		zval_add_ref(&zwritecb);
	}
	bevent->writecb = zwritecb;
		
	zval_add_ref(&zerrorcb);
	bevent->errorcb = zerrorcb;

	if (zarg) {
		zval_add_ref(&zarg);
		bevent->arg = zarg;
	} else {
		bevent->arg = NULL;
	}

	bevent->rsrc_id = zend_list_insert(bevent, le_bufferevent);
	RETURN_RESOURCE(bevent->rsrc_id);
}
/* }}} */

/* {{{ proto void event_buffer_free(resource bevent) 
 */
static PHP_FUNCTION(event_buffer_free)
{
	zval *zbevent;
	php_bufferevent_t *bevent;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zbevent) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);
	zend_list_delete(bevent->rsrc_id);
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

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rr", &zbevent, &zbase) != SUCCESS) {
		return;
	}

	ZVAL_TO_BASE(zbase, base);
	ZVAL_TO_BEVENT(zbevent, bevent);

	old_base = bevent->base;
	ret = bufferevent_base_set(base->base, bevent->bevent);

	if (ret == 0) {
		if (base != old_base) {
			/* make sure the base is destroyed after the event */
			zend_list_addref(base->rsrc_id);
		}

		if (old_base) {
			zend_list_delete(old_base->rsrc_id);
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
	long priority;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl", &zbevent, &priority) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);

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
	int data_len;
	long data_size = -1;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs|l", &zbevent, &data, &data_len, &data_size) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);

	if (ZEND_NUM_ARGS() < 3) {
		data_size = data_len;
	} else if (data_size < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "data_size cannot be less than zero");
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
	long data_size;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl", &zbevent, &data_size) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);

	if (data_size == 0) {
		RETURN_EMPTY_STRING();
	} else if (data_size < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "data_size cannot be less than zero");
		RETURN_FALSE;
	}

	data = safe_emalloc((int)data_size, sizeof(char), 1);

	ret = bufferevent_read(bevent->bevent, data, data_size);
	if (ret > 0) {
		if (ret > data_size) { /* paranoia */
			ret = data_size;
		}
		data[ret] = '\0';
		RETURN_STRINGL(data, ret, 0);
	}
	efree(data);
	RETURN_EMPTY_STRING();
}
/* }}} */

/* {{{ proto bool event_buffer_enable(resource bevent, int events) 
 */
static PHP_FUNCTION(event_buffer_enable)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	long events;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl", &zbevent, &events) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);

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
	long events;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl", &zbevent, &events) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);

	ret = bufferevent_disable(bevent->bevent, events);

	if (ret == 0) {
		RETURN_TRUE;
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
	long read_timeout, write_timeout;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rll", &zbevent, &read_timeout, &write_timeout) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);

	bufferevent_settimeout(bevent->bevent, read_timeout, write_timeout);
}
/* }}} */

/* {{{ proto void event_buffer_watermark_set(resource bevent, int events, int lowmark, int highmark) 
 */
static PHP_FUNCTION(event_buffer_watermark_set)
{
	zval *zbevent;
	php_bufferevent_t *bevent;
	long events, lowmark, highmark;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rlll", &zbevent, &events, &lowmark, &highmark) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);

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

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rr", &zbevent, &zfd) != SUCCESS) {
		return;
	}

	ZVAL_TO_BEVENT(zbevent, bevent);
	php_stream_from_zval(stream, &zfd);

	if (php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL, (void*)&fd, 1) != SUCCESS || fd < 0) {
		RETURN_FALSE;
	}

	bufferevent_setfd(bevent->bevent, fd);
}
/* }}} */


/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(libevent)
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

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(libevent)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(libevent)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(libevent)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(libevent)
{
	char buf[64];


	php_info_print_table_start();
	php_info_print_table_header(2, "libevent support", "enabled");
	php_info_print_table_row(2, "extension version", PHP_LIBEVENT_VERSION);
	php_info_print_table_row(2, "Revision", "$Revision$");
	
	snprintf(buf, sizeof(buf) - 1, "%s", event_get_version());
	php_info_print_table_row(2, "libevent version", buf);
	
	snprintf(buf, sizeof(buf) - 1, "%s", event_get_method());
	php_info_print_table_row(2, "event method used", buf);

	php_info_print_table_end();
}
/* }}} */

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
/* }}} */

/* {{{ libevent_functions[]
 */
const zend_function_entry libevent_functions[] = {
	PHP_FE(event_base_new, 				arginfo_event_new)
	PHP_FE(event_base_free, 			arginfo_event_base_loopbreak)
	PHP_FE(event_base_loop, 			arginfo_event_base_loop)
	PHP_FE(event_base_loopbreak, 		arginfo_event_base_loopbreak)
	PHP_FE(event_base_loopexit, 		arginfo_event_base_loopexit)
	PHP_FE(event_base_set, 				arginfo_event_base_set)
	PHP_FE(event_new, 					arginfo_event_new)
	PHP_FE(event_free, 					arginfo_event_del)
	PHP_FE(event_add, 					arginfo_event_add)
	PHP_FE(event_set, 					arginfo_event_set)
	PHP_FE(event_del, 					arginfo_event_del)
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
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ libevent_module_entry
 */
zend_module_entry libevent_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"libevent",
	libevent_functions,
	PHP_MINIT(libevent),
	PHP_MSHUTDOWN(libevent),
	PHP_RINIT(libevent),
	PHP_RSHUTDOWN(libevent),
	PHP_MINFO(libevent),
#if ZEND_MODULE_API_NO >= 20010901
	PHP_LIBEVENT_VERSION,
#endif
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
