/* Host tool using exactly the same envelope implementation as the payload. */
#include "catalog_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "seal") && strcmp(argv[1], "open")) ||
        strlen(argv[2]) != 1 || argv[2][0] < '1' || argv[2][0] > '3') return 2;
    size_t cap = CATALOG_SEAL_LIMIT + CATALOG_SEAL_OVERHEAD + 1;
    unsigned char *input = malloc(cap), *output = NULL;
    if (!input) return 1;
    size_t size = fread(input, 1, cap, stdin), length = 0;
    int rc = ferror(stdin) || size == cap ? -1 :
        (!strcmp(argv[1], "seal") ? catalog_seal : catalog_open)
            (input, size, (unsigned)(argv[2][0] - '0'), &output, &length);
    if (!rc && (fwrite(output, 1, length, stdout) != length || fflush(stdout))) rc = -1;
    free(input);
    free(output);
    if (rc) fputs("Catalogue envelope operation failed\n", stderr);
    return rc ? 1 : 0;
}
