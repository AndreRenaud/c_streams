#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/select.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include "streams.h"

struct stream {
	int (*read)(struct stream *stream, void *result, const int max_size);
	int (*write)(struct stream *stream, const void * const data, const int data_len);
	int (*available)(struct stream *stream, int *read, int *write);
	int (*close)(struct stream *stream);

	void (*notify)(void *data, struct stream *stream);
	void *notify_data;
};

int stream_set_notify(struct stream *stream, void (*notify)(void *data, struct stream *stream), void *data)
{
	stream->notify = notify;
	stream->notify_data = data;
	return 0;
}

static inline void stream_notify(struct stream *stream)
{
	if (stream->notify)
		stream->notify(stream->notify_data, stream);
}

static void stream_chain_notify(void *data, struct stream *parent)
{
	(void)parent;
	struct stream *child = data;
	stream_notify(child);
}

int stream_read(struct stream *stream, void *result, const int max_size)
{
	if (!stream)
		return -EINVAL;
	if (!stream->read)
		return -ENOTSUP;
	return stream->read(stream, result, max_size);
}

int stream_write(struct stream *stream, const void * const data, const int data_len)
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
	if (!stream->available) {
		/* For streams that don't support it, just assume they're always available */
		if (read) *read = stream->read ? 1 : 0;
		if (write) *write = stream->write ? 1 : 0;
		return 1;
	}
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

static int mem_read(struct stream *stream, void *result, const int max_size)
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

	if (mem->pos < mem->len)
		stream_notify(stream);

	return size;
}

static int mem_write(struct stream *stream, const void * const data, const int data_len)
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

	if (mem->pos < mem->len)
		stream_notify(stream);

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

static inline void check_notify_fd(struct stream *stream, int fd)
{
	if (!stream->notify)
		return;
	fd_set rfds;
	fd_set wfds;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_SET(fd, &rfds);
	FD_SET(fd, &wfds);

	if (select(fd + 1, &rfds, &wfds, NULL, NULL) > 0)
		stream_notify(stream);
}

static int file_read(struct stream *stream, void *result, const int max_size)
{
	FILE *fp = stream_to_file(stream);
	int e = fread(result, 1, max_size, fp);
	if (e < 0)
		return -errno;
	check_notify_fd(stream, fileno(fp));
	return e;
}

static int file_write(struct stream *stream, const void * const data, const int data_len)
{
	FILE *fp = stream_to_file(stream);
	int e = fwrite(data, 1, data_len, fp);
	if (e < 0)
		return -errno;

	check_notify_fd(stream, fileno(fp));
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

static int rand_read(struct stream *stream, void *result, int max_size)
{
	struct rand_stream *rs = stream_to_rand(stream);
	uint8_t *r8 = result;

	if (rs->max_len >= 0 && max_size > rs->max_len - rs->pos)
		max_size = rs->max_len - rs->pos;
	for (int i = 0; i < max_size; i++)
		*r8++ = rand();
	rs->pos += max_size;
	stream_notify(stream);
	return max_size;
}

struct stream *stream_rand_open(int max_len)
{
	struct stream *stream = calloc(sizeof(struct stream) + sizeof(struct rand_stream), 1);
	if (!stream)
		return NULL;
	struct rand_stream *rs = stream_to_rand(stream);
	stream->read = rand_read;
	rs->max_len = max_len;
	rs->pos = 0;
	return stream;
}

struct pipe_stream {
	int max_size;
	int used;
	char buffer[0];
};

static struct pipe_stream *stream_to_pipe(struct stream *stream)
{
	return (struct pipe_stream *)(stream + 1);
}

static int pipe_read(struct stream *stream, void *result, int max_size)
{
	struct pipe_stream *pipe = stream_to_pipe(stream);
	if (max_size > pipe->used)
		max_size = pipe->used;
	memcpy(result, pipe->buffer, max_size);
	pipe->used -= max_size;
	memcpy(pipe->buffer, &pipe->buffer[max_size], pipe->used);

	stream_notify(stream);

	return max_size;
}

static int pipe_available(struct stream *stream, int *read, int *write)
{
	struct pipe_stream *pipe = stream_to_pipe(stream);
	if (read) *read = pipe->used > 0;
	if (write) *write = pipe->used < pipe->max_size;
	return 1;
}

static int pipe_write(struct stream *stream, const void * const data, const int data_len)
{
	struct pipe_stream *pipe = stream_to_pipe(stream);
	int free_space = pipe->max_size - pipe->used;
	int read_len;
	if (data_len > free_space)
		read_len = free_space;
	else
		read_len = data_len;

	memcpy(&pipe->buffer[pipe->used], data, read_len);
	pipe->used += read_len;

	stream_notify(stream);

	return read_len;
}

struct stream *stream_pipe_open(int buffer_size)
{
	struct stream *stream = calloc(sizeof(struct stream) + sizeof(struct pipe_stream) + buffer_size, 1);
	if (!stream)
		return NULL;
	struct pipe_stream *pipe = stream_to_pipe(stream);
	pipe->max_size = buffer_size;
	pipe->used = 0;
	stream->read = pipe_read;
	stream->write = pipe_write;
	stream->available = pipe_available;

	return stream;
}

struct line_stream {
	struct stream *parent;
	int pos;
	char buffer[1024];
	int break_pos;
};

static struct line_stream *stream_to_line(struct stream *stream)
{
	return (struct line_stream *)(stream + 1);
}

static bool is_linebreak(char ch)
{
	return (ch == '\r' || ch == '\n' || ch == '\0');
}

static int line_read(struct stream *stream, void *result, int max_size)
{
	struct line_stream *line = stream_to_line(stream);
	int line_len = 0;

	/* If we don't have a line break, then read more data */
	if (line->break_pos == -1) {
		int e = stream_read(line->parent, &line->buffer[line->pos], sizeof(line->buffer) - line->pos);
		if (e < 0)
			return e;
		line->pos += e;

		for (int i = 0; i < line->pos; i++) {
			if (is_linebreak(line->buffer[i])) {
				line->break_pos = i;
				break;
			}
		}
	}

	if (line->break_pos >= 0) {
		line_len = line->break_pos;
		char *r_ch = result;
		if (line_len > max_size - 1)
			line_len = max_size - 1;
		memcpy(result, line->buffer, line_len);
		r_ch[line_len] = '\0';

		/* Absorb '\r\n' as one item */
		if (line->buffer[line->break_pos] == '\r' && 
			line->break_pos + 1 < line->pos && 
			line->buffer[line->break_pos + 1] == '\n')
			line->break_pos++;
		memcpy(line->buffer, &line->buffer[line->break_pos + 1], line->pos - line->break_pos - 1);
		line->pos -= line->break_pos + 1;
		line->break_pos = -1;

		for (int i = 0; i < line->pos; i++) {
			if (is_linebreak(line->buffer[i])) {
				line->break_pos = i;
				break;
			}
		}
	}

	stream_notify(stream);

	return line_len;
}

static int line_available(struct stream *stream, int *read, int *write)
{
	struct line_stream *line = stream_to_line(stream);
	if (read) *read = line->break_pos != -1;
	if (write) *write = 0;
	if (line->pos > 0)
		return 1;
	return stream_available(line->parent, NULL, NULL);
}

static int line_close(struct stream *stream)
{
	struct line_stream *line = stream_to_line(stream);
	stream_set_notify(line->parent, NULL, NULL);
	return 0;
}

struct stream *stream_line_open(struct stream *input)
{
	if (!input->read)
		return NULL;
	struct stream *stream = calloc(sizeof(struct stream) + sizeof(struct line_stream), 1);
	if (!stream)
		return NULL;
	struct line_stream *line = stream_to_line(stream);

	line->parent = input;
	line->pos = 0;
	line->break_pos = -1;
	stream->read = line_read;
	stream->available = line_available;
	stream->close = line_close;

	stream_set_notify(line->parent, stream_chain_notify, stream);

	return stream;
}

struct process_stream {
	pid_t pid;
	int fd;
};

struct process_stream *stream_to_process(struct stream *stream)
{
	return (struct process_stream *)(stream + 1);
}

static int process_read(struct stream *stream, void *result, int max_size)
{
	struct process_stream *process = stream_to_process(stream);
	int ret = read(process->fd, result, max_size);
	if (ret < 0)
		return -errno;
	check_notify_fd(stream, process->fd);
	return ret;
}

static int process_write(struct stream *stream, const void * const data, const int data_len)
{
	struct process_stream *process = stream_to_process(stream);
	int ret = write(process->fd, data, data_len);
	if (ret < 0)
		return -errno;
	check_notify_fd(stream, process->fd);
	return ret;
}

static int process_close(struct stream *stream)
{
	struct process_stream *process = stream_to_process(stream);
	kill(process->pid, SIGTERM);
	sleep(2);
	kill(process->pid, SIGKILL);
	waitpid(process->pid, NULL, 0);
	close(process->fd);
	return 0;
}

struct stream *stream_process_open(char * const *args)
{
	pid_t pid;
	int fd;

	pid = forkpty(&fd, NULL, NULL, NULL);

	if (pid < 0) {
		return NULL;
	} else if (pid == 0) {
		// This is the child process
        execvp(args[0], args);
        return NULL;
    } 

	struct stream *stream = calloc(sizeof(struct stream) + sizeof(struct process_stream), 1);
	if (!stream) {
		close(fd);
		kill(pid, SIGKILL);
		return NULL;
	}
	struct process_stream *process = stream_to_process(stream);
	process->pid = pid;
	process->fd = fd;
	stream->write = process_write;
	stream->read = process_read;
	stream->close = process_close;
	stream->available = NULL; // TODO: Implement available + notify
	return stream;
}

struct tcp_stream {
	int fd;
};

struct tcp_stream *stream_to_tcp(struct stream *stream)
{
	return (struct tcp_stream *)(stream + 1);
}

static int tcp_read(struct stream *stream, void *result, int max_size)
{
	struct tcp_stream *tcp = stream_to_tcp(stream);

	int n = recv(tcp->fd, result, max_size, 0);
	if (n < 0)
		return -errno;

	check_notify_fd(stream, tcp->fd);

	return n;
}

static int tcp_available(struct stream *stream, int *read, int *write)
{
	(void)stream;
	// TODO: Actually detect if we can read/write
	if (read) *read = 1;
	if (write) *write = 1;
	return 1;
}

static int tcp_write(struct stream *stream, const void * const data, const int data_len)
{
	struct tcp_stream *tcp = stream_to_tcp(stream);

	int n = send(tcp->fd, data, data_len, 0);
	if (n < 0)
		return -errno;
	check_notify_fd(stream, tcp->fd);
	return n;
}

static int tcp_close(struct stream *stream)
{
	struct tcp_stream *tcp = stream_to_tcp(stream);
	if (close(tcp->fd) < 0)
		return -errno;
	return 0;
}

struct stream *stream_tcp_open(const char *host, int port)
{
	int sockfd;
	struct hostent *he;
	struct sockaddr_in addr = {};
	int enable = 1;

	he = gethostbyname(host);
	if (!he) {
		perror("gethostbyname");
		return NULL;
	}

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return NULL;
	}

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
		perror("SO_REUSEADDR");

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr = *((struct in_addr *)he->h_addr);

	if (connect(sockfd, (struct sockaddr *)&addr, sizeof(struct sockaddr)) == -1) {
		perror("connect");
		close(sockfd);
		return NULL;
	}

	struct stream *stream = calloc(sizeof(struct stream) + sizeof(struct tcp_stream), 1);
	struct tcp_stream *tcp = stream_to_tcp(stream);
	tcp->fd = sockfd;

	stream->read = tcp_read;
	stream->write = tcp_write;
	stream->available = tcp_available;
	stream->close = tcp_close;

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
