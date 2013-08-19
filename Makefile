CPPFLAGS=-O3

all:lcov++

lcov++:lcov++.cpp demangle.cpp

clean:
	rm lcov++

