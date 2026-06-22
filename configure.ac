AC_INIT([nginx-http-read-request-body-module], [0.0.0], [bug-automake@gnu.org])
AM_INIT_AUTOMAKE([-Wall -Werror foreign -Wno-portability -Wno-unsupported])
AC_PROG_CXX
AM_PROG_AR

AC_ENABLE_STATIC(no)
AC_CONFIG_MACRO_DIR([m4])
AC_PROG_LIBTOOL

AC_SUBST([EXTRA_CFLAGS], ["-Wall -O3"])
AC_ARG_ENABLE(debug,
	[AS_HELP_STRING([--enable-debug], [enable debug @<:@default=no@:>@])],
	[ if test "${enable_debug:-no}" != no; then EXTRA_CFLAGS="${EXTRA_CLAGS} -Wall -g -O0 -fno-inline"; fi ],
	[])

AC_PREFIX_DEFAULT([/usr])
AC_CONFIG_COMMANDS_PRE([
if test "$sysconfdir" = '${prefix}/etc'; then sysconfdir=/etc; fi;
if test "$localstatedir" = '${prefix}/var'; then localstatedir=/var; fi
])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
	Makefile
])

AC_OUTPUT
