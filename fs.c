#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "fs.h"

/* Bitmap container */
typedef uint64_t fs_bitmap_word_t;
typedef struct {
    fs_bitmap_word_t w;
} fs_bitmap_t;
static const size_t fs_bitmap_bits = sizeof(fs_bitmap_word_t) * CHAR_BIT;
static inline size_t fs_bitmap_words(size_t n) {
    return (n + fs_bitmap_bits - 1) / fs_bitmap_bits;
}
static inline size_t fs_bitmap_sizeof(size_t bits) {
    return fs_bitmap_words(bits) * sizeof(fs_bitmap_word_t);
}
static inline fs_bitmap_t *fs_bitmap_create(size_t bits) {
    const size_t size = fs_bitmap_sizeof(bits);
    return memset(malloc(size), 0xFF, size);
}
static inline fs_bitmap_word_t *fs_bitmap_word(fs_bitmap_t *bm, size_t index) {
    return &bm[index / fs_bitmap_bits].w;
}
static inline fs_bitmap_word_t fs_bitmap_wordbit(size_t index) {
    return 1UL << (index % fs_bitmap_bits);
}
static inline void fs_bitmap_set(fs_bitmap_t *bm, size_t index) {
    *fs_bitmap_word(bm, index) |= fs_bitmap_wordbit(index);
}
static inline void fs_bitmap_unset(fs_bitmap_t *bm, size_t index) {
    *fs_bitmap_word(bm, index) &= ~fs_bitmap_wordbit(index);
}
static inline bool fs_bitmap_test(fs_bitmap_t *bm, size_t index) {
    return !!(*fs_bitmap_word(bm, index) & fs_bitmap_wordbit(index));
}
static size_t fs_bitmap_find_set(const fs_bitmap_t *bm, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (bm[i].w > 0)
            return i * fs_bitmap_bits + __builtin_ctz(bm[i].w);
    return -1;
}

/* Raw filesystem */
typedef struct {
    char    magic[5];
    size_t  blocksize;
    size_t  boff;
    size_t  soff;
    size_t  blocks;
} fs_header_t;

typedef struct {
    size_t next;
    size_t index;
    char   data[];
} fs_block_t;

struct fs_s {
    fs_header_t  header;
    FILE        *file;
    char        *filename;
    fs_bitmap_t *bitmap;
    size_t       blocks; /* Blocks left on device */
};

static size_t fs_size_round(size_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if (sizeof(size_t) == 8)
        v |= v >> 32;
    v++;
    return v;
}

fs_t *fs_format(const char *filename, size_t blocksize, size_t blocks) {
    fs_t *fs = malloc(sizeof(*fs));
    if (!fs)
        return NULL;

    if (blocksize < 4096)
        blocksize = 4096;
    if (blocks < 8)
        blocks = 8;

    blocksize = fs_size_round(blocksize);
    blocks    = fs_size_round(blocks);

    if (!(fs->file = fopen(filename, "ab+")))
        goto fs_format_error_fopen;
    if (!(fs->filename = strdup(filename)))
        goto fs_format_error_strdup;
    if (!(fs->bitmap = fs_bitmap_create(blocks)))
        goto fs_format_error_bitmap;

    size_t boff = sizeof(fs_header_t) + ((sizeof(fs_block_t) + blocksize) * blocks);
    size_t soff = boff + fs_bitmap_sizeof(blocks);

    fs_header_t head = {
        .magic    = "HMVFS",
        .blocksize = blocksize,
        .boff     = boff,
        .soff     = soff,
        .blocks   = blocks
    };
    memcpy(&fs->header, &head, sizeof(fs_header_t));
    fs->blocks = blocks;

    /* Write empty blocks and bitset */
    fseek(fs->file, 0, SEEK_SET);
    fwrite(&head, sizeof(head), 1, fs->file);
    for (size_t i = 0; i < blocks; i++)
        for (size_t j = 0; j < sizeof(fs_block_t) + blocksize; j++)
            fputc(0x00, fs->file);
    size_t bytes = fs_bitmap_sizeof(blocks);
    for (size_t i = 0; i < bytes; i++)
        fputc(0xFF, fs->file);

    return fs;

fs_format_error_bitmap:
    free(fs->bitmap);
fs_format_error_strdup:
    fclose(fs->file);
fs_format_error_fopen:
    free(fs);
    return NULL;
}

void fs_close(fs_t *fs) {
    fclose(fs->file);
    free(fs->filename);
    free(fs);
}

void fs_flush(fs_t *fs) {
    (void)fs;
    /* TODO: flush write queue */
    /* TODO: flush bitmap */
    /* TODO: flush strintable */
}

void fs_grow(fs_t *fs, size_t blocks) {
    /* Update header */
    size_t lastblocks = fs->header.blocks;
    fs->header.blocks = fs_size_round(fs->header.blocks + blocks);
    fs->header.boff = sizeof(fs_header_t) + ((sizeof(fs_block_t) + fs->header.blocksize) * fs->header.blocks);
    fs->header.soff = fs->header.boff + fs_bitmap_sizeof(fs->header.blocks);
    fseek(fs->file, 0, SEEK_SET);
    fwrite(&fs->header, sizeof(fs_header_t), 1, fs->file);

    /* Write new empty blocks */
    fseek(fs->file, sizeof(fs_header_t) + ((sizeof(fs_block_t) + fs->header.blocksize) * lastblocks), SEEK_SET);
    for (size_t i = 0; i < fs->header.blocks - lastblocks; i++)
        for (size_t j = 0; j < sizeof(fs_block_t) + fs->header.blocksize; j++)
            fputc(0x00, fs->file);

    fs->blocks += blocks;
    printf("grew file system %zu => %zu\n", lastblocks, fs->header.blocks);
}

void fs_write(fs_t *fs, unsigned char *data, size_t bytes) {
    size_t blockcount = (bytes <= fs->header.blocksize) ? 1 : bytes / fs->header.blocksize;
    if (blockcount > fs->blocks)
        fs_grow(fs, blockcount);

    fs_block_t *blocks = calloc(1, (sizeof(fs_block_t) + fs->header.blocksize) * blockcount);
    for (size_t i = 0; i < blockcount; i++) {
        blocks[i].index = fs_bitmap_find_set(fs->bitmap, fs->header.blocks);
        fs_bitmap_unset(fs->bitmap, blocks[i].index);
        if (i != 0 && i - 1 != blockcount) {
            blocks[i].next = fs_bitmap_find_set(fs->bitmap, fs->header.blocks);
            memcpy(blocks[i].data, data, fs->header.blocksize);
            data  += fs->header.blocksize;
            bytes /= fs->header.blocksize;
        } else {
            /* Last one or the only one */
            memcpy(blocks[i].data, data, bytes);
            blocks[i].next = blocks[i].index;
        }
        fs->blocks--;
    }

    /* TODO:
     *  put blocks into a 'to write queue' which is flushed via
     *  fs_flush.
     */

    free(blocks);
}

static void fs_debug(fs_t *fs) {
    printf("blocksize:    %zu\n", fs->header.blocksize);
    printf("bitmap table: %zu\n", fs->header.boff);
    printf("string table: %zu\n", fs->header.soff);
    printf("blocks:       %zu\n", fs->header.blocks);

    printf("blocks used:  %zu\n", fs->header.blocks - fs->blocks);
    printf("blocks left:  %zu\n", fs->blocks);

    printf("bitmap:\n");
    for (size_t i = 0; i < fs_bitmap_words(fs->header.blocks); i++) {
        fs_bitmap_word_t w = fs->bitmap[i].w;
        printf("  ");
        while (w) {
            printf((w & 1) ? "1" : "0");
            w >>= 1;
        }
        printf("\n");
    }
}

int main(void) {
    fs_t *fs = fs_format("file.fs", 4096, 8); /* make a new one or format existing */
    fs_write(fs, (unsigned char *)"hello", 5);
    fs_write(fs, (unsigned char *)"world", 5);
    fs_write(fs, (unsigned char *)"what",  4);

    /* generate something larger than blocksize and force grow */
    char *data = malloc(fs->header.blocksize * 10);
    for (size_t i = 0; i < fs->header.blocksize * 10; i++)
        data[i] = rand() % 0xFF;
    fs_write(fs, (unsigned char *)data, fs->header.blocksize * 10);

    fs_debug(fs);
}
