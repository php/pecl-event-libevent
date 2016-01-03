--TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/2
--SKIPIF--
<?php
if (!extension_loaded("libevent")) die("skip pecl/libevent needed");
--FILE--
<?php
$base = event_base_new ();
$e = event_new();
?>
Done
--EXPECTF--
Done