#include "r2_input.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char buffer[8];
    size_t length = 0;

    FILE *stream = tmpfile();
    assert(stream != NULL);
    assert(fwrite("abc\n", 1, 4, stream) == 4);
    assert(fwrite("abcd\nok\n", 1, 8, stream) == 8);
    rewind(stream);

    assert(r2_read_bounded_line(stream, buffer, 3, &length) == R2_INPUT_LINE_OK);
    assert(length == 3 && strcmp(buffer, "abc") == 0);

    assert(r2_read_bounded_line(stream, buffer, 3, &length) == R2_INPUT_LINE_TOO_LONG);
    assert(length == 3);
    assert(r2_read_bounded_line(stream, buffer, 3, &length) == R2_INPUT_LINE_OK);
    assert(length == 2 && strcmp(buffer, "ok") == 0);
    assert(r2_read_bounded_line(stream, buffer, 3, &length) == R2_INPUT_LINE_EOF);
    fclose(stream);

    stream = tmpfile();
    assert(stream != NULL);
    {
        const unsigned char binary_line[] = {'a', '\0', 'b', '\n'};
        assert(fwrite(binary_line, 1, sizeof(binary_line), stream) == sizeof(binary_line));
    }
    rewind(stream);
    assert(r2_read_bounded_line(stream, buffer, 3, &length) ==
           R2_INPUT_LINE_EMBEDDED_NUL);
    fclose(stream);

    stream = tmpfile();
    assert(stream != NULL);
    assert(fwrite("last", 1, 4, stream) == 4);
    rewind(stream);
    assert(r2_read_bounded_line(stream, buffer, 3, &length) == R2_INPUT_LINE_TOO_LONG);
    fclose(stream);

    puts("Bounded input reader tests passed.");
    return 0;
}
