pkglib_LTLIBRARIES = ngx_http_read_request_body_module.la
pkglibdir = $(libdir)/nginx/modules

AM_CFLAGS = @EXTRA_CFLAGS@

ngx_http_read_request_body_module_la_SOURCES = ngx_http_read_request_body_module.c

ngx_http_read_request_body_module_la_CFLAGS = \
	$(AM_CFLAGS) \
	-I $(top_srcdir) \
	-I /usr/include/nginx/core \
	-I /usr/include/nginx \
	-I /usr/include/nginx/stream \
	-I /usr/include/nginx/http \
	-I /usr/include/nginx/http/v2 \
	-I /usr/include/nginx/http/v3 \
	-I /usr/include/nginx/http/modules \
	-I /usr/include/nginx/http/modules/perl \
	-I /usr/include/nginx/core \
	-I /usr/include/nginx/misc \
	-I /usr/include/nginx/os \
	-I /usr/include/nginx/os/unix \
	-I /usr/include/nginx/mail \
	-I /usr/include/nginx/event \
	-I /usr/include/nginx/event/modules \
	-I /usr/include/nginx/event/quic \
	#

ngx_http_read_request_body_module_la_LDFLAGS = -version-info 2:0:2 -module -rpath $(libdir)/nginx/modules

dist_modconf_DATA = mod-http-read-request-body.conf
modconfdir = $(datadir)/nginx/modules-available
