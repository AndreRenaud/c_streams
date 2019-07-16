#ifndef STREAMS_H
#define STREAMS_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct stream;

/**
 * Open a file on the local filesystem as a stream
 * @param file_name local file name
 * @param mode Mode to open the file in, ie: "w", "r", "wr"
 * @return NULL on failure, stream handle on success
 */
struct stream *stream_file_open(const char *file_name, const char *mode);
struct stream *stream_mem_open(void *memory_area, size_t memory_len, const char *mode);
struct stream *stream_url_open(const char *url, const char *mode);
struct stream *stream_rand_open(int max_len);

/**
 * Create a stream which sends data from writes out to reads
 */
struct stream *stream_pipe_open(int buffer_size);

/**
 * Sets a callback function + userdata to be called whenever this stream
 * has data availe for either read or write (use stream_available to check which) 
 * TODO: Distinguish between read/write availability?
 */
int stream_set_notify(struct stream *stream, void (*)(void *data, struct stream *stream), void *data);

/**
 * read callback will read up to max_size bytes into the 'result' buffer
 * @return < 0 on failure, number of bytes written to result on success
 */
int stream_read(struct stream *stream, void *result, const int max_size);

/**
 * write data up to 'data_len' bytes from the 'data' pointer.
 * @return < 0 on failure, number of bytes written to result on success
 */
int stream_write(struct stream *stream, const void * const data, const int data_len);

/**
 * Close the metadata associated with thes streaming functions
 * Note: after this has been called, no further callback functions
 * may be called
 */
int stream_close(struct stream *stream);

/**
 * Returns an indicator of the number of bytes available for reading/writing
 * If it is not possible to determine the number of bytes, but it is
 * known that *some* space is available, the values should be set to MAXINT
 * NOTE: read or write may be null pointers if the caller is not using them
 * @return < 0 on failure, == 0 if the stream is finished, > 0 on success
 */
int stream_available(struct stream *stream, int *read, int *write);

/**
 * Reads all the data from ont stream and pushes it into another
 */
int stream_copy(struct stream *input_stream, struct stream *output_stream);

#endif
