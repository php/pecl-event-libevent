 --TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/9
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
  
  // free events
  event_del($event);
  event_free($event);
  unset($event);
?>
--EXPECTF--
tick
