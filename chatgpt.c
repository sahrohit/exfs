#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

#define SEGMENT_SIZE (1024 * 1024) // 1 MiB
#define BLOCK_SIZE 4096
#define SLOTS_PER_SEGMENT 255  // (1 MiB − 4096) / 4096
#define BITMAP_SIZE BLOCK_SIZE // we’ll store 255 bytes + padding

// inode structure (must be ≤4096 B)
typedef struct
{
    uint32_t type;
    uint64_t size;
    uint32_t direct_blocks[MAX_DIRECT_BLOCKS];
    uint32_t single_indirect;
    uint32_t double_indirect;
    // pad out to fill exactly 4096 B if needed...
} inode_t;

// open (and if needed create+initialize) segment file "prefixN"
static int
open_segment(const char *prefix, int segnum)
{
    char fname[64];
    snprintf(fname, sizeof fname, "%s%d", prefix, segnum);
    int fd = open(fname, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
    {
        perror("open");
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        perror("fstat");
        close(fd);
        return -1;
    }
    if (st.st_size == 0)
    {
        // brand new: size it to 1 MiB and zero the bitmap
        if (ftruncate(fd, SEGMENT_SIZE) < 0)
        {
            perror("ftruncate");
            close(fd);
            return -1;
        }
        uint8_t zero[BITMAP_SIZE] = {0};
        if (pwrite(fd, zero, BITMAP_SIZE, 0) < 0)
        {
            perror("init bitmap");
            close(fd);
            return -1;
        }
    }
    return fd;
}

// find & allocate a free slot in an inode or data segment
// prefix should be "inodeseg" or "dataseg"
static int
allocate_slot(const char *prefix)
{
    for (int seg = 0;; seg++)
    {
        int fd = open_segment(prefix, seg);
        if (fd < 0)
            return -1;

        uint8_t bitmap[BITMAP_SIZE];
        if (pread(fd, bitmap, BITMAP_SIZE, 0) != BITMAP_SIZE)
        {
            perror("read bitmap");
            close(fd);
            return -1;
        }
        // scan only the first SLOTS_PER_SEGMENT bytes
        for (int i = 0; i < SLOTS_PER_SEGMENT; i++)
        {
            if (bitmap[i] == 0)
            {
                // mark used
                bitmap[i] = 1;
                if (pwrite(fd, bitmap, BITMAP_SIZE, 0) != BITMAP_SIZE)
                {
                    perror("update bitmap");
                    close(fd);
                    return -1;
                }
                close(fd);
                return seg * SLOTS_PER_SEGMENT + i;
            }
        }
        // no free slot here, try next segment
        close(fd);
    }
}

// read or write a particular inode
static int
read_inode(int inode_num, inode_t *out)
{
    int seg = inode_num / SLOTS_PER_SEGMENT;
    int index = inode_num % SLOTS_PER_SEGMENT;
    int fd = open_segment("inodeseg", seg);
    if (fd < 0)
        return -1;
    off_t off = BITMAP_SIZE + (off_t)index * BLOCK_SIZE;
    ssize_t r = pread(fd, out, sizeof *out, off);
    close(fd);
    return (r == sizeof *out ? 0 : -1);
}

static int
write_inode(int inode_num, const inode_t *in)
{
    int seg = inode_num / SLOTS_PER_SEGMENT;
    int index = inode_num % SLOTS_PER_SEGMENT;
    int fd = open_segment("inodeseg", seg);
    if (fd < 0)
        return -1;
    off_t off = BITMAP_SIZE + (off_t)index * BLOCK_SIZE;
    ssize_t r = pwrite(fd, in, sizeof *in, off);
    close(fd);
    return (r == sizeof *in ? 0 : -1);
}

// similarly for data blocks: you’ll store exactly 4096 B at each slot
static int
read_block(int block_num, void *buf)
{
    int seg = block_num / SLOTS_PER_SEGMENT;
    int index = block_num % SLOTS_PER_SEGMENT;
    int fd = open_segment("dataseg", seg);
    if (fd < 0)
        return -1;
    off_t off = BITMAP_SIZE + (off_t)index * BLOCK_SIZE;
    ssize_t r = pread(fd, buf, BLOCK_SIZE, off);
    close(fd);
    return (r == BLOCK_SIZE ? 0 : -1);
}

static int
write_block(int block_num, const void *buf)
{
    int seg = block_num / SLOTS_PER_SEGMENT;
    int index = block_num % SLOTS_PER_SEGMENT;
    int fd = open_segment("dataseg", seg);
    if (fd < 0)
        return -1;
    off_t off = BITMAP_SIZE + (off_t)index * BLOCK_SIZE;
    ssize_t r = pwrite(fd, buf, BLOCK_SIZE, off);
    close(fd);
    return (r == BLOCK_SIZE ? 0 : -1);
}
