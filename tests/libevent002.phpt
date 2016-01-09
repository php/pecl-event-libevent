--TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/2
--SKIPIF--
<?php
if (!extension_loaded("libevent")) die("skip pecl/libevent needed");
--FILE--
<?php
$f = function() {};
$fd = fopen('https://raw.githubusercontent.com/expressif/pecl-event-libevent/master/tests/input.txt', 'r');
$base = event_base_new ();
$e = event_new();
var_dump(event_set($e, $fd, EV_READ | EV_PERSIST, $f)));
var_dump(event_base_set($e, $base));
?>
Done
--EXPECTF--
bool(true)
bool(true)
Done