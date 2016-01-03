--TEST--
pecl/libevent - general
--SKIPIF--
<?php
if (!extension_loaded("libevent")) die("skip pecl/libevent needed");
--FILE--
<?php
/* poll STDIN using basic API */

function foo($fd, $events, $arg)
{
	static $i;

	$i++;

	if ($i == 10) {
		event_base_loopexit($arg[1]);
	}
	var_dump(fread($fd, 1000));
}


$base = event_base_new();
$event = event_new();

$fd = STDIN;

var_dump(event_set($event, $fd, EV_READ | EV_PERSIST, "foo", array($event, $base)));
var_dump(event_set($event, $fd, EV_READ | EV_PERSIST, "foo", array($event, $base)));

event_base_set($event, $base);

var_dump(event_add($event));
var_dump(event_base_loop($base));
?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
int(-1)
