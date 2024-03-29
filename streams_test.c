#include <pthread.h>
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
    uint8_t
        output[2048]; // Make it bigger to make sure we don't over-read input

    rand_data(input, sizeof(input));

    struct stream *input_stream = stream_mem_open(input, sizeof(input), "r");
    TEST_CHECK(input_stream != NULL);

    struct stream *output_stream =
        stream_mem_open(output, sizeof(output), "w");
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

    TEST_CHECK(stream_write(file_stream, input, sizeof(input)) ==
               sizeof(input));
    TEST_CHECK(stream_close(file_stream) >= 0);

    file_stream = stream_file_open(filename, "r");
    TEST_CHECK(file_stream != NULL);

    TEST_CHECK(stream_read(file_stream, output, sizeof(output)) ==
               sizeof(output));
    TEST_CHECK(stream_close(file_stream) >= 0);

    TEST_CHECK(unlink(filename) >= 0);

    TEST_CHECK(memcmp(input, output, sizeof(input)) == 0);
}

struct thread_data {
    struct stream *stream;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

static void notify_unlock(void *data, struct stream *stream)
{
    struct thread_data *l = data;

    (void)stream;

    pthread_mutex_lock(&l->mutex);
    pthread_cond_broadcast(&l->cond);
    pthread_mutex_unlock(&l->mutex);
}

/* Thread function that blocks waiting for data to be available */
static void *read_thread(void *data)
{
    struct thread_data *l = data;
    int read_available = 0;
    char buffer[8];

    pthread_mutex_lock(&l->mutex);
    while (stream_available(l->stream, &read_available, NULL),
           read_available <= 0) {
        pthread_cond_wait(&l->cond, &l->mutex);
    }
    pthread_mutex_unlock(&l->mutex);
    int e = stream_read(l->stream, buffer, 4);
    if (e != 4) {
        return (void *)1;
    }
    if (strcmp(buffer, "foo") != 0) {
        return (void *)1;
    }
    return NULL;
}

void test_condition(void)
{
    struct thread_data l = {
        NULL,
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_COND_INITIALIZER,
    };
    l.stream = stream_pipe_open(1024);
    pthread_t thread;
    void *retval = NULL;

    TEST_CHECK(l.stream != NULL);
    TEST_CHECK(stream_set_notify(l.stream, notify_unlock, &l) >= 0);
    TEST_CHECK(pthread_create(&thread, NULL, read_thread, &l) >= 0);

    sleep(1); // Give the thread time to get blocking
    TEST_CHECK(stream_write(l.stream, "foo", 4) == 4);

    TEST_CHECK(pthread_join(thread, &retval) >= 0);
    TEST_CHECK(retval == NULL);
    TEST_CHECK(stream_close(l.stream) >= 0);
}

void test_line_reader(void)
{
    char input_data[] = "line 1\n"
                        "line 2\r\n"
                        "\n"
                        "line 4\n";
    char buffer[80];
    struct stream *input;
    struct stream *line;

    input = stream_mem_open(input_data, sizeof(input_data), "r");
    TEST_CHECK(input != NULL);

    line = stream_line_open(input);
    TEST_CHECK(line != NULL);

    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 6);
    TEST_CHECK(strcmp(buffer, "line 1") == 0);
    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 6);
    TEST_CHECK(strcmp(buffer, "line 2") == 0);

    /* Data should still be available */
    TEST_CHECK(stream_available(line, NULL, NULL) == 1);

    /* Absorb the blank line */
    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 0);

    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 6);
    TEST_CHECK(strcmp(buffer, "line 4") == 0);

    /* The nul byte is considered an empty line */
    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 0);
    TEST_CHECK(stream_available(line, NULL, NULL) == 0);

    stream_close(line);
    stream_close(input);
}

void test_process(void)
{
    char buffer[1024];
    char *args[] = {"printf", "foo\\nblah\\n", NULL};

    struct stream *proc = stream_process_open(args);
    TEST_CHECK(proc != NULL);

    struct stream *line = stream_line_open(proc);
    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 3);
    TEST_CHECK(strcmp(buffer, "foo") == 0);
    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 4);
    TEST_CHECK(strcmp(buffer, "blah") == 0);

    stream_close(line);
    stream_close(proc);
}

void test_process_interactive(void)
{
    char buffer[1024];
    char *args[] = {"sh", "-c", "read foo ; echo -${foo}-", NULL};
    struct stream *proc = stream_process_open(args);
    TEST_CHECK(proc != NULL);

    struct stream *line = stream_line_open(proc);
    stream_write(proc, "wibble\n", 7);
    /* Read back the echo */
    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 6);
    TEST_CHECK(strcmp(buffer, "wibble") == 0);
    /* Read back the modified response */
    TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 8);
    TEST_CHECK(strcmp(buffer, "-wibble-") == 0);

    stream_close(line);
    stream_close(proc);
}

void test_tcp(void)
{
    struct stream *tcp;
    char buffer[1024];

    system("yes | nohup nc -l 13370 &");
    sleep(1);

    tcp = stream_tcp_open("localhost", 13370);
    TEST_CHECK(tcp != NULL);
    struct stream *line = stream_line_open(tcp);

    for (int i = 0; i < 100; i++) {
        TEST_CHECK(stream_read(line, buffer, sizeof(buffer)) == 1);
        TEST_CHECK(strcmp(buffer, "y") == 0);
    }
    stream_close(line);
    stream_close(tcp);
}

TEST_LIST = {{"mem", test_mem},
             {"file", test_file},
             {"condition", test_condition},
             {"line", test_line_reader},
             {"process", test_process},
             {"process_interactive", test_process_interactive},
             {"tcp", test_tcp},
             {NULL, NULL}};