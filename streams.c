#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/select.h>

#include "streams.h"

struct stream {
	int (*read)(struct stream *stream, uint8_t *result, const int max_size);
	int (*write)(struct stream *stream, const uint8_t * const data, const int data_len);
	int (*available)(struct stream *stream, int *read, int *write);
	int (*close)(struct stream *stream);

	pthread_cond_t *cond;
};

static inline void check_cond_fd(int fd, pthread_cond_t *cond)
{
	if (!cond)
		return;
	fd_set rfds;
	fd_set wfds;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_SET(fd, &rfds);
	FD_SET(fd, &wfds);

	if (select(fd + 1, &rfds, &wfds, NULL, NULL) > 0)
		pthread_cond_broadcast(cond);
}

int stream_set_notify(struct stream *stream, pthread_cond_t *cond)
{
	stream->cond = cond;
	return 0;
}

int stream_read(struct stream *stream, uint8_t *result, const int max_size)
{
	if (!stream)
		return -EINVAL;
	if (!stream->read)
		return -ENOTSUP;
	return stream->read(stream, result, max_size);
}

int stream_write(struct stream *stream, const uint8_t * const data, const int data_len)
{
	if (!stream)
		return -EINVAL;
	if (!stream->write)
		return -ENOTSUP;
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
	if (!stream)
		return -EINVAL;
	if (!stream->available)
		return -ENOTSUP;
	return stream->available(stream, read, write);
}

struct mem_stream {
	uint8_t *base;
	size_t len;
	size_t pos;
};

static struct mem_stream *stream_to_mem(struct stream *stream)
{
	return (struct mem_stream *)(stream + 1);
}

static int mem_read(struct stream *stream, uint8_t *result, const int max_size)
{
	struct mem_stream *mem = stream_to_mem(stream);
	int size = max_size;
	int remaining = mem->len - mem->pos;

	if (remaining <= 0)
		return 0;

	if (size > remaining)
		size = remaining;

	memmove(result, mem->base + mem->pos, size);
	mem->pos += size;

	if (stream->cond && mem->pos < mem->len)
		pthread_cond_broadcast(stream->cond);

	return size;
}

static int mem_write(struct stream *stream, const uint8_t * const data, const int data_len)
{
	struct mem_stream *mem = stream_to_mem(stream);
	int size = data_len;
	int remaining = mem->len - mem->pos;

	if (remaining <= 0)
		return 0;

	if (size > remaining)
		size = remaining;

	memmove(mem->base + mem->pos, data, size);
	mem->pos += size;

	if (stream->cond && mem->pos < mem->len)
		pthread_cond_broadcast(stream->cond);

	return size;
}

static int mem_available(struct stream *stream, int *read, int *write)
{
	struct mem_stream *mem = stream_to_mem(stream);
	if (read) *read = stream->read ? mem->len - mem->pos : 0;
	if (write) *write = stream->write ? mem->len - mem->pos : 0;
	return (mem->pos == mem->len) ? 0 : 1;
}

struct stream *stream_mem_open(void *memory_area, size_t memory_len, const char *mode)
{
	struct stream *stream = calloc(sizeof(struct stream) + sizeof(struct mem_stream), 1);
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

static inline FILE *stream_to_file(struct stream *stream)
{
	return *(FILE **)(stream + 1);
}

static int file_read(struct stream *stream, uint8_t *result, const int max_size)
{
	FILE *fp = stream_to_file(stream);
	int e = fread(result, 1, max_size, fp);
	if (e < 0)
		return -errno;
	check_cond_fd(fileno(fp), stream->cond);
	return e;
}

static int file_write(struct stream *stream, const uint8_t * const data, const int data_len)
{
	FILE *fp = stream_to_file(stream);
	int e = fwrite(data, 1, data_len, fp);
	if (e < 0)
		return -errno;

	check_cond_fd(fileno(fp), stream->cond);
	return e;
}

static int file_close(struct stream *stream)
{
	FILE *fp = stream_to_file(stream);
	return fclose(fp);
}

struct stream *stream_file_open(const char *file_name, const char *mode)
{
	FILE *fp = fopen(file_name, mode);
	if (!fp)
		return NULL;
	struct stream *stream = calloc(sizeof(struct stream) + sizeof(FILE **), 1);
	if (!stream) {
		fclose(fp);
		return NULL;
	}
	*(FILE **)(stream + 1) = fp;
	stream->write = strchr(mode, 'w') ? file_write : NULL;
	stream->read = strchr(mode, 'r') ? file_read : NULL;
	stream->close = file_close;
	stream->available = NULL;
	return stream;
}

struct rand_stream {
	int max_len;
	int pos;
};

static inline struct rand_stream *stream_to_rand(struct stream *stream)
{
	return (struct rand_stream *)(stream + 1);
}

static int rand_read(struct stream *stream, uint8_t *result, int max_size)
{
	struct rand_stream *rs = stream_to_rand(stream);

	if (rs->max_len >= 0 && max_size > rs->max_len - rs->pos)
		max_size = rs->max_len - rs->pos;
	for (int i = 0; i < max_size; i++)
		*result++ = rand();
	rs->pos += max_size;
	if (stream->cond)
		pthread_cond_broadcast(stream->cond);
	return max_size;
}

struct stream *stream_rand_open(int max_len)
{
	struct stream *stream = calloc(sizeof(struct stream) + sizeof(struct rand_stream), 1);
	if (!stream)
		return NULL;
	struct rand_stream *rs = (struct rand_stream *)(stream + 1);
	stream->read = rand_read;
	rs->max_len = max_len;
	rs->pos = 0;
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
