/* rompack — assemble a bookboyadvance ROM by concatenation:
 *   stub.gba (gbafix'd code) + [magic + offset table + data blobs]
 * The GBA header checksum only covers 0xA0-0xBC, so appending is free.
 * The web converter does exactly this in JS with the same prebuilt stub.
 *
 * Usage: rompack <stub.gba> <out.gba> <font> <books> <library> <mini> <title>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN_TABLE 256
#define ROM_LIMIT (32 * 1024 * 1024)
#define TABLE_MAX 0x1FFF00          /* scan bound in the runtime's data_init */

/* explicit little-endian u32 — the GBA reads these as aligned LE loads, so
 * the packer must not depend on host endianness */
static void w32le(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}

static long copy_file(FILE *out, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "rompack: cannot open %s\n", path); exit(1); }
    char buf[65536];
    long n = 0;
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, r, out);
        n += (long)r;
    }
    fclose(f);
    return n;
}

static long pad_to(FILE *out, long pos, long align)
{
    while (pos % align) {
        fputc(0, out);
        pos++;
    }
    return pos;
}

int main(int argc, char **argv)
{
    if (argc != 9) {
        fprintf(stderr, "usage: rompack stub.gba out.gba font books library "
                        "mini title art\n");
        return 1;
    }
    const char *blob_paths[6] = {argv[3], argv[4], argv[5], argv[6], argv[7],
                                 argv[8]};
    long sizes[6];
    for (int i = 0; i < 6; i++) {
        FILE *f = fopen(blob_paths[i], "rb");
        if (!f) { fprintf(stderr, "rompack: missing %s\n", blob_paths[i]);
                  return 1; }
        fseek(f, 0, SEEK_END);
        sizes[i] = ftell(f);
        fclose(f);
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "rompack: cannot write %s\n", argv[2]);
                return 1; }
    long pos = copy_file(out, argv[1]);
    pos = pad_to(out, pos, ALIGN_TABLE);
    long table_at = pos;
    if (table_at >= TABLE_MAX) {
        fprintf(stderr, "rompack: stub too large, table at 0x%lX exceeds "
                        "runtime scan bound 0x%X\n", table_at, TABLE_MAX);
        return 1;
    }

    /* offsets are relative to the table start; blobs 4-aligned */
    uint32_t offs[6];
    long cur = 64;                  /* sizeof table block, padded */
    for (int i = 0; i < 6; i++) {
        offs[i] = (uint32_t)cur;
        cur += (sizes[i] + 3) & ~3l;
    }
    if (table_at + cur > ROM_LIMIT) {   /* check before writing a huge file */
        fprintf(stderr, "rompack: ROM too large: %ld > %d\n",
                table_at + cur, ROM_LIMIT);
        return 1;
    }

    /* DataTable (64 bytes): magic(8) + version(4) + 5 offsets(20) +
     * default_pal(4) + art_off(4) = 40 bytes, then zero pad to 64. */
    fwrite("BKBYDAT1", 1, 8, out);
    w32le(out, 1);                  /* version */
    for (int i = 0; i < 5; i++)
        w32le(out, offs[i]);        /* font books library mini title */
    w32le(out, 0);                  /* default_pal: native ROMs unset */
    w32le(out, offs[5]);            /* art_off */
    for (long i = 40; i < 64; i++)  /* pad the 40 bytes written up to 64 */
        fputc(0, out);
    pos = table_at + 64;
    for (int i = 0; i < 6; i++) {
        pos += copy_file(out, blob_paths[i]);
        pos = pad_to(out, pos, 4);
    }
    fclose(out);

    if (pos > ROM_LIMIT) {
        fprintf(stderr, "rompack: ROM too large: %ld > %d\n", pos, ROM_LIMIT);
        return 1;
    }
    printf("rompack: %s = %ld bytes (table at 0x%lX)\n", argv[2], pos,
           table_at);
    return 0;
}
