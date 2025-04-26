/*  ─────────────────────────────────  segments.h  ───────────────────────────────── */
#ifndef EXFS2_SEGMENTS_H
#define EXFS2_SEGMENTS_H

#include <stdint.h>
#include <stdio.h>

#define SEG_SIZE_BYTES (1u << 20)   /* 1 MiB segment                   */
#define OBJ_SIZE_BYTES 4096u        /* inode or data-block size        */
#define OBJ_PER_SEG 255u            /* 4 KiB × 255 + 4 KiB bitmap ≈ 1 MiB */
#define BITMAP_BYTES OBJ_SIZE_BYTES /* whole first block is bitmap   */

/* in-memory inode – exactly one OBJ_SIZE_BYTES on disk */
#define MAX_DIRECT_BLOCKS 10
typedef struct
{
    uint32_t type;
    uint64_t size;
    uint32_t direct_blocks[MAX_DIRECT_BLOCKS];
    uint32_t single_indirect;
    uint32_t double_indirect;
} inode_t;

/* on-disk data-block wrapper (convenience only) */
typedef struct
{
    char data[OBJ_SIZE_BYTES];
} datablock_t;

/* exported helpers */
int exfs2_init_storage(void);
uint32_t exfs2_alloc_inode(uint32_t type); /* returns global inode number   */
uint32_t exfs2_alloc_datablock(void);      /* returns global block number   */

#endif /* EXFS2_SEGMENTS_H */
