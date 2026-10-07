/* Test the real receiver while refusing large test allocations. */
#define _GNU_SOURCE
#include "np.h"
#include "np_file_rpc.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned allocations;
static void *bounded_test_malloc(size_t size) {
    allocations++;
    if (size > 4096) { errno = ENOMEM; return NULL; }
    return malloc(size);
}
#define malloc bounded_test_malloc
#include "../np.c"
#undef malloc

static void check_header(uint64_t length, uint8_t status, int expected_error) {
    int pipe_fd[2];
    assert(pipe(pipe_fd) == 0);
    unsigned char bytes[19] = {'N', 'P', 'A', 'G', NP_AGENT_WIRE_VERSION,
                              status, 3, 0, '0', '.', '2'};
    np_file_put64(bytes + 11, length);
    assert(np_write_full(pipe_fd[1], bytes, sizeof(bytes)) == 0);
    close(pipe_fd[1]);
    np_agent_hdr header;
    errno = 0;
    int result = np_agent_recv_hdr(pipe_fd[0], &header);
    close(pipe_fd[0]);
    if (expected_error) {
        assert(result == -1 && errno == expected_error);
    } else {
        assert(result == 0 && header.payload_len == length);
        assert(strcmp(header.version, "0.2") == 0);
    }
}

int main(void) {
    check_header(1, NP_STATUS_FILE, 0);
    check_header(NP_MAX_AGENT_MEMORY + 1, NP_STATUS_FILE, 0);
    check_header(5ULL * 1024 * 1024 * 1024, NP_STATUS_FILE, 0);
    check_header(UINT64_MAX, NP_STATUS_FILE, 0);
    check_header(0, NP_STATUS_FILE, 0);
    check_header(1, NP_STATUS_NOTFOUND + 1, EPROTO);
    assert(allocations == 0);

    const uint64_t invalid_lengths[] = {NP_MAX_AGENT_MEMORY + 1, UINT64_MAX};
    for (size_t i = 0; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); i++) {
        unsigned char *output = NULL;
        size_t output_length = 0;
        errno = 0;
        assert(np_agent_recv_payload_mem(-1, invalid_lengths[i], &output, &output_length) == -1);
        assert(errno == EMSGSIZE && output == NULL && output_length == 0);
        assert(allocations == 0);
    }

    int stream[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
    unsigned char end[8]; np_file_put64(end, 3);
    assert(np_file_send(stream[1], NP_FILE_DATA, 0, 0, "abc", 3) == 0);
    assert(np_file_send(stream[1], NP_FILE_END, 0, 0, end, sizeof(end)) == 0);
    unsigned char *output = NULL;
    size_t output_length = 0;
    assert(np_agent_recv_payload_mem(stream[0], 3, &output, &output_length) == 0);
    assert(output_length == 3 && memcmp(output, "abc", 3) == 0 && allocations == 1);
    free(output);
    close(stream[0]); close(stream[1]);

    /* A zero-byte header still requires the NPFR END(0); EOF is incomplete. */
    char directory[] = "/tmp/nativepipe-empty-receive.XXXXXX";
    assert(mkdtemp(directory));
    char destination[256];
    assert(snprintf(destination, sizeof(destination), "%s/empty", directory) < (int)sizeof(destination));
    assert(np_write_file(destination, "old", 3, 0644) == 0);
    for (int complete = 0; complete < 2; complete++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
        unsigned char header_bytes[16] = {'N', 'P', 'A', 'G', NP_AGENT_WIRE_VERSION,
                                         NP_STATUS_FILE, 0, 0};
        assert(np_write_full(stream[1], header_bytes, sizeof(header_bytes)) == 0);
        np_file_put64(end, 0);
        if (complete) assert(np_file_send(stream[1], NP_FILE_END, 0, 0, end, sizeof(end)) == 0);
        close(stream[1]);
        np_agent_hdr header;
        assert(np_agent_recv_hdr(stream[0], &header) == 0 && header.payload_len == 0);
        int result = np_agent_recv_payload_file(stream[0], header.payload_len, destination, 0644);
        assert(result == (complete ? 0 : -1));
        close(stream[0]);
        struct stat info;
        assert(stat(destination, &info) == 0 && info.st_size == (complete ? 0 : 3));
        assert((info.st_mode & 0777) == 0644);
    }
    assert(unlink(destination) == 0 && rmdir(directory) == 0);

    /* Even an empty memory result consumes and validates END's exact count. */
    for (int valid_end = 0; valid_end < 2; valid_end++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
        np_file_put64(end, valid_end ? 0 : 1);
        assert(np_file_send(stream[1], NP_FILE_END, 0, 0, end, sizeof(end)) == 0);
        output = NULL; output_length = 0;
        errno = 0;
        int result = np_agent_recv_payload_mem(stream[0], 0, &output, &output_length);
        assert(result == (valid_end ? 0 : -1));
        if (valid_end) {
            assert(output != NULL && output_length == 0);
            free(output);
        } else assert(errno == EPROTO && output == NULL);
        close(stream[0]); close(stream[1]);
    }
    puts("NPAG UInt64 headers, bounded memory and complete empty NPFR receives PASS");
    return 0;
}
