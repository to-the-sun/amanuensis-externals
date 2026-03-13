#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Mock sysmem_newptr
void *sysmem_newptr(size_t size) {
    return malloc(size);
}

// Mock sysmem_freeptr
void sysmem_freeptr(void *ptr) {
    free(ptr);
}

unsigned char *lazyvst_base64_decode(const char *in, size_t *out_len) {
    static const int B64index[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1,  0, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };

    const char *p = in;
    const char *dot = strchr(in, '.');
    if (dot) {
        // Check if prefix is numeric
        int is_numeric = 1;
        for (const char *q = in; q < dot; q++) {
            if (*q < '0' || *q > '9') {
                is_numeric = 0;
                break;
            }
        }
        if (is_numeric) p = dot + 1;
    }

    size_t in_len = strlen(p);
    if (in_len == 0) return NULL;

    size_t max_out_len = (in_len * 3) / 4 + 2;
    unsigned char *out = (unsigned char *)sysmem_newptr(max_out_len);
    if (!out) return NULL;

    size_t j = 0;
    uint32_t buffer = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len; i++) {
        int val = B64index[(unsigned char)p[i]];
        if (val != -1) {
            buffer = (buffer << 6) | val;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[j++] = (buffer >> bits) & 0xFF;
            }
        } else if (p[i] == '=') {
            break;
        }
    }

    *out_len = j;
    return out;
}

int main() {
    // Test 1: Standard Base64 with prefix
    const char *test1 = "6.SGVsbG8h";
    size_t len1 = 0;
    unsigned char *dec1 = lazyvst_base64_decode(test1, &len1);
    if (dec1) {
        printf("Test 1: %zu bytes, content: %.*s\n", len1, (int)len1, dec1);
        if (len1 == 6 && memcmp(dec1, "Hello!", 6) == 0) printf("PASSED\n"); else printf("FAILED\n");
        sysmem_freeptr(dec1);
    } else printf("Test 1: NULL returned\n");

    // Test 2: Dots representing zeros (4 dots = 3 bytes of 0x00)
    const char *test2 = "4....."; // prefix '4', separator '.', data '....'
    size_t len2 = 0;
    unsigned char *dec2 = lazyvst_base64_decode(test2, &len2);
    if (dec2) {
        printf("Test 2: %zu bytes\n", len2);
        if (len2 == 3 && dec2[0] == 0 && dec2[1] == 0 && dec2[2] == 0) printf("PASSED\n"); else printf("FAILED\n");
        sysmem_freeptr(dec2);
    } else printf("Test 2: NULL returned\n");

    // Test 3: Mixed content from user example
    const char *test3 = "2678.CMlaKA....fQPMDZ";
    size_t len3 = 0;
    unsigned char *dec3 = lazyvst_base64_decode(test3, &len3);
    if (dec3) {
        printf("Test 3: %zu bytes\n", len3);
        if (len3 > 0) printf("PASSED\n"); else printf("FAILED\n");
        sysmem_freeptr(dec3);
    } else printf("Test 3: NULL returned\n");

    // Test 4: Dot without numeric prefix (should NOT skip)
    const char *test4 = ".SGVsbG8h";
    size_t len4 = 0;
    unsigned char *dec4 = lazyvst_base64_decode(test4, &len4);
    if (dec4) {
        // First char is '.' -> 0. Remaining SGVsbG8h -> 6 chars.
        // 7 chars total.
        printf("Test 4: %zu bytes\n", len4);
        if (len4 > 0) printf("PASSED\n"); else printf("FAILED\n");
        sysmem_freeptr(dec4);
    } else printf("Test 4: NULL returned\n");

    return 0;
}
