#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "streams.h"

struct stream {
	int (*read)(struct stream *stream, uint8_t *result, const int max_size);
	int (*write)(struct stream *stream, const uint8_t * const data, const int data_len);
	int (*available)(struct stream *stream, int *read, int *write);
	int (*close)(struct stream *stream);
};

int stream_read(struct stream *stream, uint8_t *result, const int max_size)
{
	if (!stream || !stream->read)
		return -EINVAL;
	return stream->read(stream, result, max_size);
}

int stream_write(struct stream *stream, const uint8_t * const data, const int data_len)
{
	if (!stream || !stream->write)
		return -EINVAL;
	return stream->write(stream, data, data_len);
}

int stream_close(struct stream *stream)
{
	int ret = 0;
	if (!stream)
		return -EINVAL;
	if (stream->close)
		ret = stream->close(stream);
	free(stream);
	return ret;
}

int stream_available(struct stream *stream, int *read, int *write)
{
	if (!stream->available)
		return -EINVAL;
	return stream->available(stream, read, write);
}

struct mem_stream {
	uint8_t *base;
	size_t len;
	size_t pos;
};

static int mem_read(struct stream *stream, uint8_t *result, const int max_size)
{
	struct mem_stream *mem = (struct mem_stream *)(stream + 1);
	int size = max_size;
	int remaining = mem->len - mem->pos;

	if (remaining <= 0)
		return 0;

	if (size > remaining)
		size = remaining;

	memmove(result, mem->base + mem->pos, size);
	mem->pos += size;

	return size;
}

static int mem_write(struct stream *stream, const uint8_t * const data, const int data_len)
{
	struct mem_stream *mem = (struct mem_stream *)(stream + 1);
	int size = data_len;
	int remaining = mem->len - mem->pos;

	if (remaining <= 0)
		return 0;

	if (size > remaining)
		size = remaining;

	memmove(mem->base + mem->pos, data, size);
	mem->pos += size;

	return size;
}

static int mem_available(struct stream *stream, int *read, int *write)
{
	struct mem_stream *mem = (struct mem_stream *)(stream + 1);
	if (read) *read = stream->read ? mem->len - mem->pos : 0;
	if (write) *write = stream->write ? mem->len - mem->pos : 0;
	return (mem->pos == mem->len) ? 0 : 1;
}

struct stream *stream_mem_open(void *memory_area, size_t memory_len, const char *mode)
{
	struct stream *stream = malloc(sizeof(struct stream) + sizeof(struct mem_stream));
	if (!stream)
		return NULL;
	struct mem_stream *mem = (struct mem_stream *)(stream + 1);

	mem->base = memory_area;
	mem->len = memory_len;
	mem->pos = 0;
	stream->write = strchr(mode, 'w') ? mem_write : NULL;
	stream->read = strchr(mode, 'r') ? mem_read : NULL;
	stream->available = mem_available;

	return stream;
}

static int file_read(struct stream *stream, uint8_t *result, const int max_size)
{
	FILE *fp = *(FILE **)(stream + 1);
	return fread(result, 1, max_size, fp);
}

static int file_write(struct stream *stream, const uint8_t * const data, const int data_len)
{
	FILE *fp = *(FILE **)(stream + 1);
	return fwrite(data, 1, data_len, fp);
}

static int file_close(struct stream *stream)
{
	FILE *fp = *(FILE **)(stream + 1);
	return fclose(fp);
}

struct stream *stream_file_open(const char *file_name, const char *mode)
{
	FILE *fp = fopen(file_name, mode);
	if (!fp)
		return NULL;
	struct stream *stream = malloc(sizeof(struct stream) + sizeof(FILE **));
	if (!stream) {
		fclose(fp);
		return NULL;
	}
	*(FILE **)(stream + 1) = fp;
	stream->write = strchr(mode, 'w') ? file_write : NULL;
	stream->read = strchr(mode, 'r') ? file_read : NULL;
	stream->close = file_close;
	return stream;
}

/*******
 * UTILITY FUNCTIONS
 *******/
int stream_copy(struct stream *input_stream, struct stream *output_stream)
{
	uint8_t buffer[1024];
	int copied = 0;
	bool done = false;

	while (!done) {
		int r = stream_read(input_stream, buffer, sizeof(buffer));
		if (r < 0)
			return r;
		if (r == 0)
			done = true;

		int w = 0;
		while (w < r) {
			int t = stream_write(output_stream, &buffer[w], r - w);
			if (t < 0)
				return t;
			if (t == 0) {
				done = true;
				break;
			}
			w += t;
		}
		copied += w;
	}

	return copied;
}
