--TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/8
--SKIPIF--
<?php
if (!extension_loaded("libevent")) die("skip pecl/libevent needed");
--FILE--
$base = event_base_new();
$event = event_new();

event_set($event, 0, EV_TIMEOUT, function() {
echo "function called";
});
event_base_set($event, $base);

var_dump(@event_add($event, 5000000));
var_dump(event_base_loop($base));
?>
--EXPECTF--
bool(false)
int(0)
