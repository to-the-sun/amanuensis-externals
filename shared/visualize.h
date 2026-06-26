#ifndef VISUALIZE_H
#define VISUALIZE_H

#include <stddef.h>

// Call this in your object's 'new' function
// Returns 0 on success, non-zero on failure
int visualize_init(void *x);

// Call this in your object's 'free' function
void visualize_cleanup();

// Call this to send a message
void visualize(void *x, const char *message);

// Call this to send a message and wait for a response line (up to 1s timeout)
// Returns the number of bytes received, or -1 on error.
int visualize_exchange(void *x, const char *message, char *response, size_t response_size);

#endif // VISUALIZE_H
