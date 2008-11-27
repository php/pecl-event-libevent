<?php

/* poll STDIN using basic API */

function foo($fd, $events, $arg)
{
	static $i;

	$i++;

	if ($i == 10) {
		event_base_loopexit($arg[1]);
	}
	var_dump(fread($fd, 1000));
}


$base = event_base_new();
$event = event_new();

$fd = STDIN;

var_dump(event_set($event, $fd, EV_READ | EV_PERSIST, "foo", array($event, $base)));
var_dump(event_set($event, $fd, EV_READ | EV_PERSIST, "foo", array($event, $base)));

event_base_set($event, $base);

var_dump(event_add($event));
var_dump(event_base_loop($base));

exit;

/* poll STDIN using event_buffer API */

function foo2($buf, $arg)
{
	static $i;

	$i++;

	if ($i == 10) {
		event_base_loopexit($arg);
	}
	var_dump($buf);
	var_dump(event_buffer_read($buf, 10));
}

$base = event_base_new();
$b = event_buffer_new(STDIN, "foo2", NULL, "foo2", $base);

event_buffer_base_set($b, $base);
event_buffer_enable($b, EV_READ);

event_base_loop($base);



?>
