--TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/8
--SKIPIF--
<?php
if (!extension_loaded("libevent")) die("skip pecl/libevent needed");
--FILE--
<?php

  function tick() {
    echo 'tick';
  }

  // create ressources
  $base = event_base_new();
  $event = event_timer_new();

  // configure
  event_timer_set($event, 'tick');
  event_base_set($event, $base);
  event_add($event, 1000);

  // run
  event_base_loop($base, EVLOOP_ONCE);

  // remove event from the loop
  event_del($event);
  unset($event);

  // attach a new event on loop
  $ev2 = event_timer_new();
  event_timer_set($ev2, 'tick');
  event_base_set($ev2, $base);
  event_add($ev2, 1000);

  // run
  event_base_loop($base, EVLOOP_ONCE);

?>
--EXPECTF--
ticktick
