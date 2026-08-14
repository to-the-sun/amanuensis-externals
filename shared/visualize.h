#ifndef VISUALIZE_H
#define VISUALIZE_H

#include <stddef.h>

// Call this in your object's 'new' function
// Returns 0 on success, non-zero on failure
int visualize_init();

// Call this in your object's 'free' function
void visualize_cleanup();

// Call this to send a message
void visualize(void *x, const char *message);

// Call this to send a message and wait for a response line (up to 1s timeout)
// Returns the number of bytes received, or -1 on error.
int visualize_exchange(void *x, const char *message, char *response, size_t response_size);

// Dynamic port management for multi-instance / multi-channel visualizers
int visualize_allocate_port(int start_port);
void visualize_to_port(void *x, int port, const char *type, const char *message);
void visualize_close_port(int port);

#endif // VISUALIZE_H
