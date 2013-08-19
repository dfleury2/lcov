lcov
====

a lcov compatible C++ program (based on gcov source)

This is a work-for-me project aimed to replace the great [lcov](http://ltp.sourceforge.net/coverage/lcov.php),
but too slow for my taste.

This project works fine for a simple project configuration (a simple directory to recursively
find .gcno et .gcda files)

just type : lcov ++ /path/to/gcfiles/

A file app.find is generated which is compatible for use with others lcov tools (genhtml, ...)

I am using Linux RedHat for my tests and Windows for Debug and development, on some middle side projects (<50k LOC),
original lcov take 4 minutes to generate an app.info file, this one take less than 5 seconds. For an XP, TDD
oriented project, this is a great gain.

As of today, I am using gcov source from gcc 4.1.2. Seems that gcc guys port some codes for gcc 4.8.1 and there
is some work to be done for a clean Windows compilation.

This last version use demangle for C++ function/method names (better result for function coverage)

Hope it can help somebody else.
Regards


