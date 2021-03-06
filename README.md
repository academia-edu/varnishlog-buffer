# Varnishlog-Buffer

This program buffers the output of [varnishlog][varnishlog], thereby allowing
[other tools][avl] time to parse or otherwise handle the logs.

## Motivation

Varnish uses [shared memory][vsm] to log hits and statistics. This is very fast,
but has the difficulty that if a log consumer doesn't read the logs quickly
enough, varnish can overwrite logs which have yet to be consumed. This program
makes every attempt to read those logs before varnish has the opportunity to
overwrite them, buffering the read logs in memory until a log consumer can deal
with them at it's leisure.

## Compilation

```
make
```

### Dependencies

* [glib][glib] >= 2.32

## Usage

See `varnishlog-buffer --help`.
It must be run as root unless run with the `--low-priorty` option.

[varnishlog]: https://www.varnish-cache.org/docs/3.0/reference/varnishlog.html
[avl]: https://github.com/academia-edu/academia-varnishlog
[vsm]: https://www.varnish-cache.org/docs/trunk/reference/vsm.html
[glib]: https://developer.gnome.org/glib/stable/

<!--- vim: set tw=80: -->
