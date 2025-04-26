#define _GNU_SOURCE
/*  ─────────────────────────────────  segments.c  ───────────────────────────────── */
#include "segments.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* ─── internal helpers ───────────────────────────────────────────────────────────*/

static void seg_name(char *dst, const char *prefix, unsigned idx)
{
    sprintf(dst, "%s%u", prefix, idx);
}

static FILE *open_or_create(const char *name)
{
    FILE *fp = fopen(name, "rb+");
    if (!fp)
        fp = fopen(name, "wb+"); /* create if absent            */
    return fp;
}

static int seg_count(const char *prefix)
{
    /* count how many segment files with the prefix already exist               */
    char n[32];
    int i = 0;
    while (1)
    {
        seg_name(n, prefix, i);
        FILE *fp = fopen(n, "rb");
        if (!fp)
            break;
        fclose(fp);
        ++i;
    }
    return i; /* first missing index         */
}

static void zero_segment(FILE *fp)
{
    static char zeros[SEG_SIZE_BYTES] = {0};
    fseek(fp, 0, SEEK_SET);
    fwrite(zeros, 1, SEG_SIZE_BYTES, fp);
    fflush(fp);
}

/* ensure seg0 exists and has a zeroed bitmap with root inode marked in-use       */
static void ensure_first_inode_segment(void)
{
    char name[32];
    seg_name(name, "inodeseg", 0);
    FILE *fp = open_or_create(name);
    struct stat st;
    fstat(fileno(fp), &st);
    if (st.st_size == 0)
    { /* brand-new file → initialise */
        zero_segment(fp);

        /* mark inode 0 (root) as used inside bitmap */
        uint8_t one = 1u;
        fseek(fp, 0, SEEK_SET);
        fwrite(&one, 1, 1, fp);
        fflush(fp);
    }
    fclose(fp);
}

static void ensure_first_data_segment(void)
{
    char name[32];
    seg_name(name, "dataseg", 0);
    FILE *fp = open_or_create(name);
    struct stat st;
    fstat(fileno(fp), &st);
    if (st.st_size == 0)
        zero_segment(fp);
    fclose(fp);
}

/* look for a free bit inside prefix{idx}.  returns bit index or -1               */
static int find_free_bit(const char *prefix, unsigned *seg_idx_out)
{
    uint8_t bitmap[BITMAP_BYTES];

    for (unsigned seg = 0;; ++seg)
    {
        char name[32];
        seg_name(name, prefix, seg);
        FILE *fp = fopen(name, "rb+");
        if (!fp)
            break; /* reached end of existing segs */

        fread(bitmap, 1, BITMAP_BYTES, fp);
        for (unsigned i = 0; i < OBJ_PER_SEG; ++i)
        {
            unsigned byte = i / 8, bit = i % 8;
            if (!(bitmap[byte] & (1u << bit)))
            {
                *seg_idx_out = seg;
                fclose(fp);
                return (int)i; /* local index within segment  */
            }
        }
        fclose(fp);
    }
    return -1; /* none free in existing segs  */
}

static FILE *create_new_segment(const char *prefix, unsigned idx)
{
    char name[32];
    seg_name(name, prefix, idx);
    FILE *fp = fopen(name, "wb+");
    zero_segment(fp);
    return fp;
}

static void set_bit(FILE *fp, unsigned bit_idx)
{
    unsigned byte = bit_idx / 8, bit = bit_idx % 8;
    fseek(fp, byte, SEEK_SET);
    int b = fgetc(fp);
    if (b == EOF)
    {
        b = 0;
        fseek(fp, byte, SEEK_SET);
    }
    uint8_t v = (uint8_t)b | (1u << bit);
    fseek(fp, byte, SEEK_SET);
    fputc(v, fp);
    fflush(fp);
}

/* translate (segment, local_idx) → global number                                 */
static inline uint32_t global_no(unsigned seg, unsigned local)
{
    return seg * OBJ_PER_SEG + local;
}

/* ─── public API ────────────────────────────────────────────────────────────────*/

int exfs2_init_storage(void)
{
    ensure_first_inode_segment();
    ensure_first_data_segment();
    return 0;
}

uint32_t exfs2_alloc_inode(uint32_t type /* currently unused */)
{
    (void)type; /* avoid -Wunused-parameter for now      */
    unsigned seg_idx;
    int local = find_free_bit("inodeseg", &seg_idx);
    FILE *fp;

    if (local == -1)
    { /* need new inode segment     */
        seg_idx = seg_count("inodeseg");
        fp = create_new_segment("inodeseg", seg_idx);
        local = 0; /* first inode in the new seg */
    }
    else
    {
        char name[32];
        seg_name(name, "inodeseg", seg_idx);
        fp = fopen(name, "rb+");
    }

    set_bit(fp, local);

    /* write blank inode (caller will overwrite if desired) */
    inode_t blank = {0};
    long off = BITMAP_BYTES + (long)local * OBJ_SIZE_BYTES;
    fseek(fp, off, SEEK_SET);
    fwrite(&blank, sizeof(blank), 1, fp);
    fflush(fp);
    fclose(fp);

    /* return global inode number                                                     */
    return global_no(seg_idx, local);
}

uint32_t exfs2_alloc_datablock(void)
{
    unsigned seg_idx;
    int local = find_free_bit("dataseg", &seg_idx);
    FILE *fp;

    if (local == -1)
    { /* need new data segment      */
        seg_idx = seg_count("dataseg");
        fp = create_new_segment("dataseg", seg_idx);
        local = 0;
    }
    else
    {
        char name[32];
        seg_name(name, "dataseg", seg_idx);
        fp = fopen(name, "rb+");
    }

    set_bit(fp, local);
    fclose(fp);
    return global_no(seg_idx, local);
}
