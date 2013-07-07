dnl Process this file with autoconf to produce a configure script.
AC_INIT([vpsrecord],[1.2])
AC_CONFIG_SRCDIR([vpsrecord.c])
AM_INIT_AUTOMAKE
AM_CONFIG_HEADER(config.h)

dnl Checks for programs.
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS([_GNU_SOURCE])
AC_CHECK_FUNCS(getopt_long, ,
	       AC_MSG_ERROR(Required functions missing))
AC_CHECK_FUNCS(program_invocation_short_name)

PKG_CHECK_MODULES([zvbi], [zvbi-0.2 >= 0.2], [
				  CPPFLAGS="$CPPFLAGS $zvbi_CFLAGS"
				  LIBS="$LIBS $zvbi_LIBS" 
				],
				  AC_MSG_ERROR(ZVBI library missing))

AC_OUTPUT(Makefile)
echo -e "\nAll done. Run make now."


