/* Host-side test of the ESP-IDF mbedTLS certificate verifier. */
#include <stdint.h>
#include <stdio.h>
#include "mbedtls/x509_crt.h"

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    mbedtls_x509_crt certificate, trust;
    mbedtls_x509_crt_init(&certificate);
    mbedtls_x509_crt_init(&trust);
    int result = mbedtls_x509_crt_parse_file(&certificate, argv[1]);
    if (result == 0) result = mbedtls_x509_crt_parse_file(&trust, argv[2]);
    uint32_t flags = 0;
    if (result == 0) result = mbedtls_x509_crt_verify(&certificate, &trust, NULL, argv[3], &flags, NULL, NULL);
    mbedtls_x509_crt_free(&certificate);
    mbedtls_x509_crt_free(&trust);
    return result == 0 && flags == 0 ? 0 : 1;
}
