CPPFLAGS=-O3

all:lcov++ gcov

lcov++:lcov++.cpp
gcov:gcov.cpp

clean:
	rm lcov++ gcov

