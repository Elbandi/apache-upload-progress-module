# Process this file with autoconf to produce a configure script.
AC_INIT(mod_upload_progress, 0.3)

AC_CONFIG_SRCDIR([mod_upload_progress.c])
AC_CONFIG_HEADER([config.h])

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL

# Checks for libraries.
AC_CHECK_LIB([mysqlclient], [mysql_init])

AC_PATH_PROG(APXS2, [apxs2])

AC_ARG_WITH(mysql-includes,
AC_HELP_STRING([--with-mysql-includes], [MySQL includes directory]),
[
if test ! -e $withval/mysql.h; then
	AC_MSG_ERROR(Invalid MySQL includes directory: $withval)
else
	mysql_includes="-I$withval"
fi
],
[
# Common location finder
if test -e /usr/include/mysql/mysql.h; then
	mysql_includes="-I/usr/include/mysql"
elif test -e /usr/include/mysql.h; then
	mysql_includes="-I/usr/include"
elif test -e /usr/local/include/mysql/mysql.h; then
	mysql_includes="-I/usr/local/include/mysql"
elif test -e /usr/local/include/mysql.h; then
	mysql_includes="-I/usr/local/include"
else
	AC_MSG_ERROR(MySQL include files not found!)
fi
])
AC_SUBST(mysql_includes)

AC_ARG_WITH(mysql-libs,
AC_HELP_STRING([--with-mysql-libs], [MySQL libraries directory]),
[
if test ! -e $withval/libmysqlclient.so; then
	AC_MSG_ERROR(Invalid MySQL libs directory: $withval)
else
	mysql_libs="-L$withval"
fi
],
[
# Common location finder
if test -e /usr/lib/mysql/libmysqlclient.so; then
	mysql_libs="-L/usr/lib/mysql"
elif test -e /usr/lib/libmysqlclient.so; then
	mysql_libs="-L/usr/lib"
elif test -e /usr/local/lib/mysql/libmysqlclient.so; then
	mysql_libs="-L/usr/local/lib/mysql"
elif test -e /usr/local/lib/libmysqlclient.so ; then
	mysql_libs="-L/usr/local/lib"
else
	AC_MSG_ERROR(MySQL libraries not found!)
fi
])
AC_SUBST(mysql_libs)

AC_SUBST(build_opts, "$mysql_includes $mysql_libs")

# Checks for header files.

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST

# Checks for library functions.
AC_TYPE_SIGNAL

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
