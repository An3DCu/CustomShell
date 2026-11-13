/*
 * The libFuzzer target: any bytes at all, through the lexer and the parser.
 *
 * Built with AddressSanitizer and UBSan as well, so a crash, a read out of
 * bounds, undefined behavior or a leak all stop the run and leave the input
 * that did it. `make fuzz` runs it; see the Makefile.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "xalloc.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *text = xmalloc(size + 1);
    struct list *list = NULL;
    char err[256];

    /* The shell gets a C string: the reader drops zero bytes before this. */
    memcpy(text, data, size);
    text[size] = '\0';
    if (parse_program(text, &list, err, sizeof err) == PARSE_OK)
        list_free(list);
    free(text);
    return 0;
}
