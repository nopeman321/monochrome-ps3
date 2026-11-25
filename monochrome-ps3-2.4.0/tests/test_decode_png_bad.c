#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "gfx_engine.h"

int main(void)
{
    const uint8_t bad[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE
    };
    const uint8_t truncated_png[16] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52
    };
    static uint8_t sentinel = 0;

    uint8_t *rgba = &sentinel;
    int w = -1;
    int h = -1;
    int rc = gfx_decode_png_memory(bad, sizeof(bad), &rgba, &w, &h);
    assert(rc == -1);
    assert(rgba == NULL);

    rgba = &sentinel;
    w = -1;
    h = -1;
    rc = gfx_decode_png_memory(truncated_png, sizeof(truncated_png), &rgba, &w, &h);
    assert(rc == -1);
    assert(rgba == NULL);

    return 0;
}
