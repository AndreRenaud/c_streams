CFLAGS=-g -Wall -Wextra -pipe -O3
SOURCES=streams.c streams.h streams_test.c

default: test
.PHONY: default

%.o: %.c streams.h
	cppcheck --quiet $<
	$(CC) -c -o $@ $< $(CFLAGS)

test: streams_test
	./streams_test -t --xml-output=results.xml
.PHONY: test

streams_test: streams.o streams_test.o
	$(CC) -o $@ streams.o streams_test.o $(LFLAGS)

format:
	 for s in $(SOURCES) ; do \
		clang-format $$s | diff -u $$s - ; \
	done
.PHONY: format

help:
	echo "make <target>"
	echo "   ... test - build and run the test software"
	echo "   ... clean"

update_acutest:
	curl -o acutest.h https://raw.githubusercontent.com/mity/acutest/master/include/acutest.h
.PHONY: update_acutest

clean:
	rm streams_test *.o
