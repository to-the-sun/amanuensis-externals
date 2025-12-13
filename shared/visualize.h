#ifndef VISUALIZE_H
#define VISUALIZE_H

// Call this in your object's 'new' function
// Returns 0 on success, non-zero on failure
int visualize_init();

// Call this in your object's 'free' function
void visualize_cleanup();

// Call this to send a message
void visualize(const char *message);

#endif // VISUALIZE_H
