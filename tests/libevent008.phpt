--TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/19
--SKIPIF--
<?php
if (!extension_loaded("libevent")) die("skip pecl/libevent needed");
--FILE--
<?php
class foo {
  public function checkConnection($socket)
  {
    var_dump($socket);
  }
}

$http = stream_socket_client(
  "tcp://www.google.com:80",
  $errno, $errstr, 5, STREAM_CLIENT_ASYNC_CONNECT
);
var_dump($http, $errno, $errstr);


$base = event_base_new();
$event = event_new();
$foo = new foo();

var_dump(event_set($event, (int)$http, EV_WRITE | EV_PERSIST, array($foo, 'checkConnection') ));
var_dump(event_base_set($event, $base));
var_dump(event_add($event));
var_dump(event_base_loop($base, EVLOOP_ONCE));
?>
--EXPECTF--
resource(5) of type (stream)
int(0)
string(0) ""
bool(true)
bool(true)
bool(true)
resource(5) of type (stream)
int(0)
