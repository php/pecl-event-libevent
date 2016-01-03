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
    $base = event_base_new();
    $ev = event_buffer_new($sock, NULL, NULL, 'ec');
    event_buffer_free($ev);
    event_base_free($base);
    echo "$i\n";
}
fclose ($sock);
?>
--EXPECTF--
0
1