#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>
#include <stdint.h>

void base64_encode(const char *input, size_t input_size, void (*putc_f)(char));
void base64_encode_chunk(const char *input, size_t input_size, void (*putc_f)(char));
void base64_encode_finish(void (*putc_f)(char));
int base64_encode_buffer(const char *input, size_t input_size, char *output, size_t output_size);

#endif
