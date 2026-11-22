#ifndef PILOT_SHA256_H
#define PILOT_SHA256_H
#include <stddef.h>
#include <stdint.h>
int pilot_sha256_file(const char *path, uint64_t expected_size, uint8_t out[32]);
void pilot_sha256_bytes(const void *data, size_t size, uint8_t out[32]);
#endif
