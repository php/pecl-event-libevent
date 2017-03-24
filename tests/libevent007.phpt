--TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/14
--SKIPIF--
<?php
if (!extension_loaded("libevent")) die("skip pecl/libevent needed");
--FILE--
<?php
$base = event_base_new();
$event = event_new();

var_dump(@event_set($event, 0, EV_TIMEOUT, function() {
  var_dump("TIMEOUT");
}));
event_base_set($event, $base);

var_dump(@event_add($event, 5000000));
var_dump(event_base_loop($base));
?>
--EXPECTF--
bool(true)
bool(true)
string(7) "TIMEOUT"
int(1)
