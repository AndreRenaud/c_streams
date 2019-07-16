C Streams
======

Single-file simple C library for providing a unified read/write 
streaming interface to a number of underlying systems.
This provides the following
features:
* Simpllify upper communications layers
* Easy to stub out lower layers with dummy data for testing purposes
* Reduce inter-module dependencies by keeping the interface at the 'stream' layer

This is heavily influenced by Go's io.Reader & io.Writer interfaces

Example usage
=============

Creates a line-based stream reader by layering on top of a memory-based
stream reader.
```c
#include "c_streams.h"

int main(void) {
	char input_data[] = "line 1\n"
						"line 2\r\n"
						"\n"
						"line 4\n";
	char buffer[80];
	struct stream *input;
	struct stream *line;

	input = stream_mem_open(input_data, sizeof(input_data), "r");
	line = stream_line_open(input);
	stream_read(line, buffer, sizeof(buffer));
	assert(strcmp(buffer, "line 1") == 0);
	stream_read(line, buffer, sizeof(buffer));
	assert(strcmp(buffer, "line 2") == 0);

	stream_close(line);
	stream_close(input);
	return 0;
}
```

License
=======
[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)

The source here is public domain.
If you find it useful, please drop me a line at andre@ignavus.net.

Builds
======

[CircleCI Build](https://circleci.com/gh/AndreRenaud/c_streams) status: ![Build Status](https://circleci.com/gh/AndreRenaud/c_streams.svg)

