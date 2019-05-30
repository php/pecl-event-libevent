--TEST--
pecl/libevent - bug https://github.com/expressif/pecl-event-libevent/issues/3
--SKIPIF--
<?php
if (!extension_loaded("libevent")) die("skip pecl/libevent needed");
--FILE--
<?php
function ec() { }

$sock = stream_socket_server ('tcp://0.0.0.0:2000', $errno, $errstr);

for ($i=0; $i<2; $i++) {
    echo "$i\n";
    $base = event_base_new();
    var_dump($base);
    $ev = event_buffer_new($sock, NULL, NULL, 'ec');
    var_dump($ev);
    var_dump(event_buffer_base_set($ev, $base));
    event_buffer_free($ev);
    var_dump($ev);
    event_base_free($base);
    var_dump($base);
    unset($base);
    unset($ev);

}
fclose ($sock);
?>
--EXPECTF--
0
resource(6) of type (event base)
resource(7) of type (buffer event)
bool(true)
resource(7) of type (Unknown)
resource(6) of type (Unknown)
1
resource(8) of type (event base)
resource(9) of type (buffer event)
bool(true)
resource(9) of type (Unknown)
resource(8) of type (Unknown)