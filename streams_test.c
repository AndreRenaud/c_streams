#include <stdlib.h>

#include "acutest.h"
#include "streams.h"

static void rand_data(void *buffer, int size)
{
	uint8_t *b8 = buffer;
	while (size--)
		*b8++ = rand();
}

void test_mem(void)
{
	uint8_t input[1024];
	uint8_t output[2048]; // Make it bigger to make sure we don't over-read input

	rand_data(input, sizeof(input));

	struct stream *input_stream = stream_mem_open(input, sizeof(input), "r");
	TEST_CHECK(input_stream != NULL);

	struct stream *output_stream = stream_mem_open(output, sizeof(output), "w");
	TEST_CHECK(output_stream != NULL);

	TEST_CHECK(stream_copy(input_stream, output_stream) == sizeof(input));
	TEST_CHECK(stream_close(input_stream) >= 0);
	TEST_CHECK(stream_close(output_stream) >= 0);

	TEST_CHECK(memcmp(input, output, sizeof(input)) == 0);
}

void test_file(void)
{
	uint8_t input[1024];
	uint8_t output[1024];
	const char *filename = "/tmp/test_data";

	rand_data(input, sizeof(input));

	struct stream *file_stream = stream_file_open(filename, "w");
	TEST_CHECK(file_stream != NULL);

	TEST_CHECK(stream_write(file_stream, input, sizeof(input)) == sizeof(input));
	TEST_CHECK(stream_close(file_stream) >= 0);

	file_stream = stream_file_open(filename, "r");
	TEST_CHECK(file_stream != NULL);

	TEST_CHECK(stream_read(file_stream, output, sizeof(output)) == sizeof(output));
	TEST_CHECK(stream_close(file_stream) >= 0);

	TEST_CHECK(unlink(filename) >= 0);

	TEST_CHECK(memcmp(input, output, sizeof(input)) == 0);
}

//void test_rand(void)
//{
//	struct stream *r = stream_rand_open(1024);
//}

TEST_LIST = {
    { "mem", test_mem },
    { "file", test_file },
    { NULL, NULL }
};