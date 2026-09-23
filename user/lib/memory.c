#include <stddef.h>
#include <stdint.h>

void *memset(void *destination, int value, size_t size) {
    unsigned char *bytes = (unsigned char *)destination;
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = (unsigned char)value;
    }
    return destination;
}

void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *output = (unsigned char *)destination;
    const unsigned char *input = (const unsigned char *)source;
    for (size_t index = 0; index < size; ++index) {
        output[index] = input[index];
    }
    return destination;
}

void *memmove(void *destination, const void *source, size_t size) {
    unsigned char *output = (unsigned char *)destination;
    const unsigned char *input = (const unsigned char *)source;
    uintptr_t output_address = (uintptr_t)output;
    uintptr_t input_address = (uintptr_t)input;
    if (output_address < input_address) {
        for (size_t index = 0; index < size; ++index) {
            output[index] = input[index];
        }
    } else if (output_address > input_address) {
        while (size != 0) {
            --size;
            output[size] = input[size];
        }
    }
    return destination;
}
