Libevent bindings for PHP
=========================

[![Build Status](https://travis-ci.org/expressif/pecl-event-libevent.svg)](https://travis-ci.org/expressif/pecl-event-libevent)

Libevent is a library that provides a mechanism to execute a callback function when a specific event occurs on a file descriptor or after a timeout has been reached.

More information about Libevent can be found at » http://libevent.org/.

--
**NOTE :** This fork is the port of the module to [PHP7 / PHPNG](https://wiki.php.net/phpng-upgrading) 


## Requirements 

This extension requires [» libevent](http://libevent.org/) library. Minimal required version is 1.4.0.

## Installation 

Information for installing this PECL extension may be found in the manual chapter titled [Installation of PECL extensions](http://php.net/manual/en/install.pecl.php). Additional information such as new releases, downloads, source files, maintainer information, and a CHANGELOG, can be located here: » http://pecl.php.net/package/libevent.

## Compile from sources

Pre-requisites (only for linux) :

```sh
sudo apt-get install php7.0-dev gcc make libevent-dev
```

Compile sources :

```sh
git clone https://github.com/expressif/pecl-event-libevent.git
cd pecl-event-libevent
phpize
./configure
make && sudo make install
```

## Predefined Constants

The constants below are defined by this extension, and will only be available when the extension has either been compiled into PHP or dynamically loaded at runtime.

* EV_TIMEOUT (integer)
* EV_READ (integer)
* EV_WRITE (integer)
* EV_SIGNAL (integer)
* EV_PERSIST (integer)
* EVLOOP_NONBLOCK (integer)
* EVLOOP_ONCE (integer)

## Examples

Example #1 polling STDIN using basic API

```php
<?php

function print_line($fd, $events, $arg)
{
    static $max_requests = 0;

    $max_requests++;

    if ($max_requests == 10) {
        // exit loop after 10 writes
        event_base_loopexit($arg[1]);
    }

    // print the line
    echo  fgets($fd);
}

// create base and event
$base = event_base_new();
$event = event_new();

$fd = STDIN;

// set event flags
event_set($event, $fd, EV_READ | EV_PERSIST, "print_line", array($event, $base));
// set event base
event_base_set($event, $base);

// enable event
event_add($event);
// start event loop
event_base_loop($base);
```

Example #2 polling STDIN using buffered event API

```php
<?php

function print_line($buf, $arg)
{
    static $max_requests;

    $max_requests++;

    if ($max_requests == 10) {
        event_base_loopexit($arg);
    }

    // print the line
    echo event_buffer_read($buf, 4096);
}

function error_func($buf, $what, $arg)
{
    // handle errors
}

$base = event_base_new();
$eb = event_buffer_new(STDIN, "print_line", NULL, "error_func", $base);

event_buffer_base_set($eb, $base);
event_buffer_enable($eb, EV_READ);

event_base_loop($base);
```

## Credits

Antony Dovgal, Arnaud Le Blanc, Ioan Chiriac, JoungKyun KIm
