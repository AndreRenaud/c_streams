CFLAGS=-g -Wall -pipe -O3

default: test

%.o: %.c streams.h
	$(CC) -c -o $@ $< $(CFLAGS)

test: streams_test
	./streams_test

streams_test: streams.o streams_test.o
	$(CC) -o $@ streams.o streams_test.o $(LFLAGS)

help:
	echo "make <target>"
	echo "   ... test - build and run the test software"
	echo "   ... clean"

update_acutest:
	curl -o acutest.h https://raw.githubusercontent.com/mity/acutest/master/include/acutest.h
.PHONY: update_acutest

clean:
	rm streams_test *.o