#ifndef NP_EXEC_STREAM_H
#define NP_EXEC_STREAM_H
#include <sys/types.h>

/* Takes ownership of input_output and error_output, but not socket. stdout
 * and stderr are separate for non-PTY sessions. Reaps precisely this child. */
int np_exec_stream(int socket, int input_output, int error_output, pid_t child, int terminal);
void np_exec_cancel(pid_t child);
#endif
