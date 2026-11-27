#ifndef CRYPTO_H
#define CRYPTO_H
#include <stdint.h>
#include <stddef.h>

void aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]);
int  ct_equal(const uint8_t *a, const uint8_t *b, size_t n);   /* constant-time compare */

#endif
