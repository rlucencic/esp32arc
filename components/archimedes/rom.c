/*
 * Which OS a ROM image holds: its version string ("Arthur 1.20 (25 Sep
 * 1987)", "RISC OS 3.11 (29 Sep 1992)"), and the size it has to be.
 */
#include <stdio.h>
#include <string.h>
#include "arc_internal.h"

static const uint8_t *find(const uint8_t *rom, uint32_t size, const char *text)
{
    size_t n = strlen(text);
    for (uint32_t i = 0; i + n <= size; i++) {
        if (rom[i] == (uint8_t)text[0] && !memcmp(rom + i, text, n)) {
            return rom + i;
        }
    }
    return NULL;
}

const char *arc_rom_identify(const uint8_t *rom, uint32_t size, arc_os_t *os, char *buf, int len)
{
    static const struct {
        const char *prefix;
        uint32_t size;
        arc_os_t os;
    } known[] = {
        { "Arthur 1.", 512 * 1024, ARC_OS_ARTHUR },
        { "RISC OS 2.", 512 * 1024, ARC_OS_RISCOS2 },
        { "RISC OS 3.1", 2 * 1024 * 1024, ARC_OS_RISCOS3 },
    };
    for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
        if (size != known[k].size) {
            continue;
        }
        const uint8_t *p = find(rom, size, known[k].prefix);
        if (!p) {
            continue;
        }
        /* the version and its date: up to the closing bracket */
        int n = 0;
        while (n < len - 1 && p + n < rom + size && p[n] >= ' ' && p[n] < 0x7F) {
            buf[n] = (char)p[n];
            if (p[n++] == ')') {
                break;
            }
        }
        buf[n] = 0;
        if (os) {
            *os = known[k].os;
        }
        return buf;
    }
    return NULL;
}
