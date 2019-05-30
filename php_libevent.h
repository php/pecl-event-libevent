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

#ifndef PHP_LIBEVENT_H
#define PHP_LIBEVENT_H

#define PHP_LIBEVENT_VERSION "0.2.0-dev"

extern zend_module_entry libevent_module_entry;
#define phpext_libevent_ptr &libevent_module_entry

#ifdef ZTS
#include "TSRM.h"
#endif

#ifndef zend_always_inline
# if defined(__GNUC__)
#  define zend_always_inline inline __attribute__((always_inline))
# elif defined(_MSC_VER)
#  define zend_always_inline __forceinline
# else
#  define zend_always_inline inline
# endif
#endif

#ifndef safe_efree
#define safe_efree(x) do {if(x){efree(x); x=NULL;}} while(0)
#endif

#endif	/* PHP_LIBEVENT_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
