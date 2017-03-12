--TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/8
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
	var_dump(fread($fd, 1));
}


$base = event_base_new();

$fd = fopen('https://raw.githubusercontent.com/expressif/pecl-event-libevent/master/tests/input.txt', 'r');

// first event reader
$ev1 = event_new();
var_dump(event_set($ev1, $fd, EV_READ | EV_PERSIST, "foo", array($ev1, $base)));
var_dump(event_base_set($ev1, $base));
var_dump(event_add($ev1));
var_dump(event_base_loop($base, EVLOOP_ONCE));
var_dump(event_del($ev1));
var_dump(event_free($ev1));
unset($ev1);
var_dump($fd);

// second event reader
echo "\n2nd try\n";
$ev2 = event_new();
var_dump(event_set($ev2, $fd, EV_READ | EV_PERSIST, "foo", array($ev2, $base)));
var_dump(event_base_set($ev2, $base));
var_dump(event_add($ev2));
var_dump(event_base_loop($base, EVLOOP_ONCE));
var_dump(event_del($ev2));
var_dump(event_free($ev2));
unset($ev2);

?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
string(1) "0"
int(0)
bool(true)
NULL
resource(6) of type (stream)

2nd try
bool(true)
bool(true)
bool(true)
string(1) "1"
int(0)
bool(true)
NULL
