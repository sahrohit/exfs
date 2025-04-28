// exfs2.c
// Implementation of the ExFS2 File System

#define _GNU_SOURCE // Enable POSIX extensions like strdup, strtok_r
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h> // Now includes S_IFDIR, S_IFREG with _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>   // For basename and dirname
#include <limits.h>   // For UINT32_MAX
#include <inttypes.h> // For PRIu64 macro

// --- Constants ---
#define BLOCK_SIZE 4096
#define SEGMENT_SIZE (1 * 1024 * 1024) // 1MB
#define MAX_FILENAME_LEN 255
#define INODE_SEGMENT_PREFIX "inode_"
#define DATA_SEGMENT_PREFIX "data_"
#define SEGMENT_SUFFIX ".seg"

#define BITS_PER_BYTE 8
#define BLOCKS_PER_SEGMENT (SEGMENT_SIZE / BLOCK_SIZE)                             // 256
#define BITMAP_BLOCKS_PER_SEGMENT 1                                                // Reserve 1 block for bitmap
#define USABLE_BLOCKS_PER_SEGMENT (BLOCKS_PER_SEGMENT - BITMAP_BLOCKS_PER_SEGMENT) // 255

// Aliases for clarity
#define USABLE_INODES_PER_SEGMENT USABLE_BLOCKS_PER_SEGMENT
#define USABLE_DATA_BLOCKS_PER_SEGMENT USABLE_BLOCKS_PER_SEGMENT

#define ROOT_INODE_NUM 0

// File types for inode mode (subset of standard S_IF*)
#define EXFS2_IFREG S_IFREG // Regular file
#define EXFS2_IFDIR S_IFDIR // Directory

// File types for dirent (subset of DT_*)
#define EXFS2_DT_REG 8
#define EXFS2_DT_DIR 4
#define EXFS2_DT_UNKNOWN 0

// --- Data Structures ---

// Inode Structure (must fit within BLOCK_SIZE)
// Calculate NUM_DIRECT based on actual size
typedef struct
{
    uint16_t mode; // File type (EXFS2_IFREG, EXFS2_IFDIR)
    // uint32_t link_count;   // Add if needed for removal logic robustness
    uint64_t size;              // File size in bytes
    uint32_t direct_blocks[12]; // Placeholder - RECALCULATE BELOW
    uint32_t single_indirect;   // Block number of the single indirect block (0 if unused)
    uint32_t double_indirect;   // Block number of the double indirect block (0 if unused)
    uint32_t triple_indirect;   // Block number of the triple indirect block (0 if unused)
    // Add padding if needed to exactly fill BLOCK_SIZE
} exfs2_inode_base; // Base structure for size calculation

#define INODE_METADATA_SIZE (sizeof(uint16_t) + sizeof(uint64_t) + 3 * sizeof(uint32_t))
#define INODE_DIRECT_POINTER_SPACE (BLOCK_SIZE - INODE_METADATA_SIZE)
#define NUM_DIRECT (INODE_DIRECT_POINTER_SPACE / sizeof(uint32_t))

#define POINTERS_PER_INDIRECT_BLOCK (BLOCK_SIZE / sizeof(uint32_t)) // 1024

// Final Inode Structure
typedef struct
{
    uint16_t mode;
    uint64_t size;
    uint32_t direct_blocks[NUM_DIRECT];
    uint32_t single_indirect;
    uint32_t double_indirect;
    uint32_t triple_indirect;
    char padding[BLOCK_SIZE - INODE_METADATA_SIZE - (NUM_DIRECT * sizeof(uint32_t))]; // Ensure block alignment
} exfs2_inode;

// Directory Entry Structure
// NOTE: Using fixed-size name array here for simplicity in this example.
// A variable-length approach is more complex for block fitting.
typedef struct
{
    uint32_t inode_num;              // Inode number (0 if entry is unused)
    char name[MAX_FILENAME_LEN + 1]; // Null-terminated filename
    // Add type or length fields if implementing variable length entries
} exfs2_dirent;

#define DIRENTS_PER_BLOCK (BLOCK_SIZE / sizeof(exfs2_dirent))

// Indirect Block Structure
typedef struct
{
    uint32_t block_ptrs[POINTERS_PER_INDIRECT_BLOCK];
} indirect_block;

// --- Global State (Simplified) ---
// In a real scenario, this might be part of a mounted filesystem struct
int max_inode_segment_idx = -1;
int max_data_segment_idx = -1;

// --- Forward Declarations ---
void initialize_exfs2();
int read_block(uint32_t block_num, char *buffer);
int write_block(uint32_t block_num, const char *buffer);
int read_inode(uint32_t inode_num, exfs2_inode *inode_buf);
int write_inode(uint32_t inode_num, const exfs2_inode *inode_buf);
uint32_t allocate_inode();
uint32_t allocate_block();
void free_inode(uint32_t inode_num);
void free_block(uint32_t block_num);
int traverse_path(const char *path, uint32_t *result_inode_num, bool create_missing, char *last_component);
void list_directory(uint32_t dir_inode_num, int indent, const char *current_path_prefix);
void add_file(const char *exfs2_path, const char *local_path);
void extract_file(const char *exfs2_path);
void remove_file_or_dir(const char *exfs2_path);
void debug_path(const char *exfs2_path);
uint32_t find_entry_in_dir(uint32_t dir_inode_num, const char *name);
uint32_t get_block_num_for_file_offset(exfs2_inode *inode, uint64_t offset, bool allocate_if_needed, uint32_t file_inode_num);
// --- Added Forward Declarations ---
int add_entry_to_dir(uint32_t parent_inode_num, const char *name, uint32_t entry_inode_num);
void recursive_free(uint32_t inode_num);
// ---

// --- Utility Functions ---

void print_error(const char *msg)
{
    fprintf(stderr, "Error: %s", msg);
    if (errno)
    {
        fprintf(stderr, " (%s)", strerror(errno));
    }
    fprintf(stderr, "\n");
}

// Gets the FILE* for a segment, creating the segment file if needed
FILE *get_segment_fp(const char *prefix, int index, const char *mode, bool create_if_missing)
{
    char filename[256];
    snprintf(filename, sizeof(filename), "%s%d%s", prefix, index, SEGMENT_SUFFIX);

    FILE *fp = fopen(filename, mode);
    if (fp == NULL)
    {
        if (errno == ENOENT && create_if_missing)
        {
            // printf("Segment file '%s' not found. Creating.\n", filename);
            // Create the file, fill with zeros
            fp = fopen(filename, "wb"); // Write binary
            if (fp == NULL)
            {
                perror("Error creating segment file");
                return NULL;
            }
            char zero_buffer[1024] = {0};
            for (int i = 0; i < SEGMENT_SIZE / sizeof(zero_buffer); ++i)
            {
                if (fwrite(zero_buffer, 1, sizeof(zero_buffer), fp) != sizeof(zero_buffer))
                {
                    perror("Error writing zeros to new segment file");
                    fclose(fp);
                    return NULL;
                }
            }
            fclose(fp);
            // printf("Segment file '%s' created and zeroed.\n", filename);

            // Reopen with the requested mode
            fp = fopen(filename, mode);
            if (fp == NULL)
            {
                perror("Error reopening newly created segment file");
                return NULL;
            }
            // Update max segment index tracking
            if (strcmp(prefix, INODE_SEGMENT_PREFIX) == 0 && index > max_inode_segment_idx)
            {
                max_inode_segment_idx = index;
                // printf("Updated max_inode_segment_idx to %d\n", index);
            }
            else if (strcmp(prefix, DATA_SEGMENT_PREFIX) == 0 && index > max_data_segment_idx)
            {
                max_data_segment_idx = index;
                // printf("Updated max_data_segment_idx to %d\n", index);
            }
        }
        else
        {
            // Don't print error if just checking existence (e.g., "rb" mode)
            if (!(mode[0] == 'r' && mode[1] == 'b' && !create_if_missing))
            {
                // perror("Error opening segment file"); // This can be noisy if file doesn't exist and create_if_missing is false
            }
            return NULL;
        }
    }
    return fp;
}

// --- Bitmap Operations ---

// NOTE: These operate on a single block buffer assumed to be the bitmap block
void set_bit(char *bitmap, int bit_index)
{
    int byte_index = bit_index / BITS_PER_BYTE;
    int bit_offset = bit_index % BITS_PER_BYTE;
    if (bit_index >= USABLE_BLOCKS_PER_SEGMENT)
    {
        // fprintf(stderr, "set_bit: bit_index %d out of bounds (max %d)\n", bit_index, USABLE_BLOCKS_PER_SEGMENT -1);
        return;
    }
    bitmap[byte_index] |= (1 << bit_offset);
}

void clear_bit(char *bitmap, int bit_index)
{
    int byte_index = bit_index / BITS_PER_BYTE;
    int bit_offset = bit_index % BITS_PER_BYTE;
    if (bit_index >= USABLE_BLOCKS_PER_SEGMENT)
    {
        // fprintf(stderr, "clear_bit: bit_index %d out of bounds (max %d)\n", bit_index, USABLE_BLOCKS_PER_SEGMENT -1);
        return;
    }
    bitmap[byte_index] &= ~(1 << bit_offset);
}

bool is_bit_set(const char *bitmap, int bit_index)
{
    int byte_index = bit_index / BITS_PER_BYTE;
    int bit_offset = bit_index % BITS_PER_BYTE;
    if (bit_index >= USABLE_BLOCKS_PER_SEGMENT)
    {
        // fprintf(stderr, "is_bit_set: bit_index %d out of bounds (max %d)\n", bit_index, USABLE_BLOCKS_PER_SEGMENT -1);
        return false; // Treat out-of-bounds as not set for safety
    }
    return (bitmap[byte_index] & (1 << bit_offset)) != 0;
}

// Finds the first free bit (0) in the bitmap block buffer
// Returns bit index (0 to USABLE_BLOCKS_PER_SEGMENT-1) or -1 if full
int find_free_bit(const char *bitmap, int num_usable_items)
{
    for (int i = 0; i < num_usable_items; ++i)
    {
        if (!is_bit_set(bitmap, i))
        {
            return i; // Found a free bit
        }
    }
    return -1; // No free bits found
}

// --- Core Read/Write Operations ---

// Reads a block from the appropriate segment file
// block_num is the GLOBAL block number
int read_block(uint32_t block_num, char *buffer)
{
    int segment_index = block_num / USABLE_DATA_BLOCKS_PER_SEGMENT;
    int item_index_in_segment = block_num % USABLE_DATA_BLOCKS_PER_SEGMENT;
    int block_offset_in_segment = item_index_in_segment + BITMAP_BLOCKS_PER_SEGMENT;
    off_t offset = (off_t)block_offset_in_segment * BLOCK_SIZE;

    // printf("read_block: global_block_num=%u -> seg_idx=%d, item_idx_in_seg=%d, block_offset_in_seg=%d, file_offset=%ld\n",
    //        block_num, segment_index, item_index_in_segment, block_offset_in_segment, offset);

    FILE *fp = get_segment_fp(DATA_SEGMENT_PREFIX, segment_index, "rb", false); // Read binary
    if (!fp)
    {
        // fprintf(stderr, "read_block: Failed to get segment fp for %s%d\n", DATA_SEGMENT_PREFIX, segment_index);
        return -1;
    }

    if (fseek(fp, offset, SEEK_SET) != 0)
    {
        perror("Error seeking in data segment for read_block");
        fclose(fp);
        return -1;
    }
    size_t bytes_read = fread(buffer, 1, BLOCK_SIZE, fp);
    if (bytes_read != BLOCK_SIZE)
    {
        if (feof(fp))
        {
            fprintf(stderr, "Warning: Premature EOF reading block %u (read %zu bytes) - filling remainder with zeros\n", block_num, bytes_read);
            memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
        }
        else
        {
            perror("Error reading block from data segment");
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

// Writes a block to the appropriate segment file
// block_num is the GLOBAL block number
int write_block(uint32_t block_num, const char *buffer)
{
    int segment_index = block_num / USABLE_DATA_BLOCKS_PER_SEGMENT;
    int item_index_in_segment = block_num % USABLE_DATA_BLOCKS_PER_SEGMENT;
    int block_offset_in_segment = item_index_in_segment + BITMAP_BLOCKS_PER_SEGMENT;
    off_t offset = (off_t)block_offset_in_segment * BLOCK_SIZE;

    // printf("write_block: global_block_num=%u -> seg_idx=%d, item_idx_in_seg=%d, block_offset_in_seg=%d, file_offset=%ld\n",
    //        block_num, segment_index, item_index_in_segment, block_offset_in_segment, offset);

    FILE *fp = get_segment_fp(DATA_SEGMENT_PREFIX, segment_index, "r+b", true); // Read/Write binary
    if (!fp)
    {
        // fprintf(stderr, "write_block: Failed to get segment fp for %s%d\n", DATA_SEGMENT_PREFIX, segment_index);
        return -1;
    }

    if (fseek(fp, offset, SEEK_SET) != 0)
    {
        perror("Error seeking in data segment for write_block");
        fclose(fp);
        return -1;
    }
    if (fwrite(buffer, 1, BLOCK_SIZE, fp) != BLOCK_SIZE)
    {
        perror("Error writing block to data segment");
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

// Reads an inode from the appropriate segment file
int read_inode(uint32_t inode_num, exfs2_inode *inode_buf)
{
    int segment_index = inode_num / USABLE_INODES_PER_SEGMENT;
    int item_index_in_segment = inode_num % USABLE_INODES_PER_SEGMENT;
    int block_offset_in_segment = item_index_in_segment + BITMAP_BLOCKS_PER_SEGMENT;
    off_t offset = (off_t)block_offset_in_segment * BLOCK_SIZE;

    // printf("read_inode: inode_num=%u -> seg_idx=%d, item_idx_in_seg=%d, block_offset_in_seg=%d, file_offset=%ld\n",
    //        inode_num, segment_index, item_index_in_segment, block_offset_in_segment, offset);

    FILE *fp = get_segment_fp(INODE_SEGMENT_PREFIX, segment_index, "rb", false); // Read binary
    if (!fp)
    {
        // fprintf(stderr, "read_inode: Failed to get segment fp for %s%d\n", INODE_SEGMENT_PREFIX, segment_index);
        return -1;
    }

    if (fseek(fp, offset, SEEK_SET) != 0)
    {
        perror("Error seeking in inode segment for read_inode");
        fclose(fp);
        return -1;
    }
    size_t bytes_read = fread(inode_buf, 1, sizeof(exfs2_inode), fp);
    if (bytes_read != sizeof(exfs2_inode))
    {
        if (feof(fp))
        {
            fprintf(stderr, "Warning: Premature EOF reading inode %u (read %zu bytes)\n", inode_num, bytes_read);
            memset(inode_buf, 0, sizeof(exfs2_inode));
            fclose(fp);
            return -1;
        }
        else
        {
            perror("Error reading inode from inode segment");
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

// Writes an inode to the appropriate segment file
int write_inode(uint32_t inode_num, const exfs2_inode *inode_buf)
{
    int segment_index = inode_num / USABLE_INODES_PER_SEGMENT;
    int item_index_in_segment = inode_num % USABLE_INODES_PER_SEGMENT;
    int block_offset_in_segment = item_index_in_segment + BITMAP_BLOCKS_PER_SEGMENT;
    off_t offset = (off_t)block_offset_in_segment * BLOCK_SIZE;

    // printf("write_inode: inode_num=%u -> seg_idx=%d, item_idx_in_seg=%d, block_offset_in_seg=%d, file_offset=%ld\n",
    //        inode_num, segment_index, item_index_in_segment, block_offset_in_segment, offset);

    FILE *fp = get_segment_fp(INODE_SEGMENT_PREFIX, segment_index, "r+b", true); // Read/Write binary
    if (!fp)
    {
        // fprintf(stderr, "write_inode: Failed to get segment fp for %s%d\n", INODE_SEGMENT_PREFIX, segment_index);
        return -1;
    }

    if (fseek(fp, offset, SEEK_SET) != 0)
    {
        perror("Error seeking in inode segment for write_inode");
        fclose(fp);
        return -1;
    }
    if (fwrite(inode_buf, 1, sizeof(exfs2_inode), fp) != sizeof(exfs2_inode))
    {
        perror("Error writing inode to inode segment");
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

// --- Allocation and Deallocation ---

// Generic allocation: finds free bit, sets it, returns global number
// Returns UINT32_MAX on failure.
uint32_t allocate_generic(const char *prefix, int *max_segment_idx_ptr, int usable_items_per_seg)
{
    char bitmap_block[BLOCK_SIZE];
    int current_max_idx = *max_segment_idx_ptr;
    bool is_data_segment = (strcmp(prefix, DATA_SEGMENT_PREFIX) == 0);
    bool is_inode_segment = (strcmp(prefix, INODE_SEGMENT_PREFIX) == 0);

    // Scan existing segments first
    for (int seg_idx = 0; seg_idx <= current_max_idx; ++seg_idx)
    {
        FILE *fp = get_segment_fp(prefix, seg_idx, "r+b", false);
        if (!fp)
        {
            // This can happen if a segment was notionally created but then failed
            // or if numbering is sparse. For robustness, maybe try to open with 'true' if it was expected?
            // For now, if it doesn's exist, skip it.
            // printf("allocate_generic: Segment %s%d not found for scanning bitmap (current_max_idx=%d).\n", prefix, seg_idx, current_max_idx);
            continue;
        }

        if (fseek(fp, 0, SEEK_SET) != 0 || fread(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE)
        {
            if (feof(fp))
            {
                fprintf(stderr, "Warning: EOF reading bitmap for %s%d during allocation. Assuming full.\n", prefix, seg_idx);
                memset(bitmap_block, 0xFF, BLOCK_SIZE);
            }
            else
            {
                perror("Error reading bitmap block during allocation");
                fclose(fp);
                continue;
            }
        }

        int free_bit_index = -1;
        for (int i = 0; i < usable_items_per_seg; ++i)
        {
            if (is_inode_segment && seg_idx == 0 && i == ROOT_INODE_NUM)
                continue;
            if (is_data_segment && seg_idx == 0 && i == 0)
                continue;

            if (!is_bit_set(bitmap_block, i))
            {
                free_bit_index = i;
                break;
            }
        }

        if (free_bit_index != -1)
        {
            set_bit(bitmap_block, free_bit_index);
            if (fseek(fp, 0, SEEK_SET) != 0 || fwrite(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE)
            {
                perror("Error writing updated bitmap block");
                clear_bit(bitmap_block, free_bit_index); // Revert in-memory
                fclose(fp);
                return UINT32_MAX;
            }
            fclose(fp);
            uint32_t global_num = (uint32_t)seg_idx * usable_items_per_seg + free_bit_index;
            // printf("allocate_generic: Allocated %s number %u from segment %d, bit %d\n", prefix, global_num, seg_idx, free_bit_index);
            return global_num;
        }
        fclose(fp);
    }

    // No free slots in existing segments, create a new one
    int new_segment_idx = current_max_idx + 1;
    // printf("allocate_generic: No free slots in existing segments. Attempting to create new segment %s%d\n", prefix, new_segment_idx);

    FILE *fp = get_segment_fp(prefix, new_segment_idx, "r+b", true); // Create if missing
    if (!fp)
    {
        fprintf(stderr, "Failed to create new segment %s%d\n", prefix, new_segment_idx);
        return UINT32_MAX;
    }
    // The get_segment_fp with create_if_missing=true should have updated max_segment_idx_ptr if it was successful
    // Let's verify this after fp is confirmed.
    // if (new_segment_idx > *max_segment_idx_ptr) *max_segment_idx_ptr = new_segment_idx; // Should be handled by get_segment_fp

    memset(bitmap_block, 0, BLOCK_SIZE);
    int first_free_bit = 0; // In a new segment, bit 0 should be free
    // (unless it's inode segment 0 or data segment 0, which this 'new_segment_idx' logic won't hit if init is done)

    if (first_free_bit >= usable_items_per_seg)
    {
        fprintf(stderr, "Error: No usable bits in new segment %s%d?\n", prefix, new_segment_idx);
        fclose(fp);
        return UINT32_MAX;
    }

    set_bit(bitmap_block, first_free_bit);

    if (fseek(fp, 0, SEEK_SET) != 0 || fwrite(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE)
    {
        perror("Error writing initial bitmap to new segment");
        fclose(fp);
        // If we failed to write bitmap, segment file might exist but be unusable.
        // Potentially, we should try to remove it or mark *max_segment_idx_ptr back.
        return UINT32_MAX;
    }
    fclose(fp);

    // *max_segment_idx_ptr should have been updated by get_segment_fp.
    // If not, we might need to set it here:
    if (new_segment_idx > *max_segment_idx_ptr)
    {
        // printf("allocate_generic: Manually updating max_segment_idx_ptr for %s to %d\n", prefix, new_segment_idx);
        *max_segment_idx_ptr = new_segment_idx;
    }

    uint32_t global_num = (uint32_t)new_segment_idx * usable_items_per_seg + first_free_bit;
    // printf("allocate_generic: Allocated %s number %u from NEW segment %d, bit %d\n", prefix, global_num, new_segment_idx, first_free_bit);
    return global_num;
}

// Allocates an inode. Returns inode number > 0.
// Returns UINT32_MAX on failure.
uint32_t allocate_inode()
{
    uint32_t inode_num = allocate_generic(INODE_SEGMENT_PREFIX, &max_inode_segment_idx, USABLE_INODES_PER_SEGMENT);

    if (inode_num == UINT32_MAX)
    {
        // fprintf(stderr, "allocate_inode: Failed to allocate inode.\n");
        return UINT32_MAX;
    }

    exfs2_inode new_inode = {0};
    if (write_inode(inode_num, &new_inode) != 0)
    {
        fprintf(stderr, "Failed to zero-initialize newly allocated inode %u\n", inode_num);
        free_inode(inode_num);
        return UINT32_MAX;
    }
    return inode_num;
}

// Allocates a data block. Returns block number > 0, or 0 on failure.
uint32_t allocate_block()
{
    uint32_t block_num = allocate_generic(DATA_SEGMENT_PREFIX, &max_data_segment_idx, USABLE_DATA_BLOCKS_PER_SEGMENT);

    if (block_num == UINT32_MAX)
    {
        // fprintf(stderr, "allocate_block: Failed to allocate block.\n");
        return 0;
    }

    char zero_buffer[BLOCK_SIZE] = {0};
    if (write_block(block_num, zero_buffer) != 0)
    {
        fprintf(stderr, "Failed to zero-initialize newly allocated block %u\n", block_num);
        free_block(block_num);
        return 0;
    }
    return block_num;
}

// Frees an item (inode or block) in the bitmap
void free_generic(uint32_t global_num, const char *prefix, int usable_items_per_seg)
{
    bool is_inode = (strcmp(prefix, INODE_SEGMENT_PREFIX) == 0);
    if (is_inode && global_num == ROOT_INODE_NUM)
    {
        fprintf(stderr, "Warning: Attempt to free root inode %u - operation ignored.\n", ROOT_INODE_NUM);
        return;
    }
    if (!is_inode && global_num == 0)
    {
        fprintf(stderr, "Warning: Attempt to free data block 0 - operation ignored.\n");
        return;
    }

    int segment_index = global_num / usable_items_per_seg;
    int bit_index_in_segment = global_num % usable_items_per_seg;

    int current_max_idx = is_inode ? max_inode_segment_idx : max_data_segment_idx;
    if (segment_index > current_max_idx || segment_index < 0)
    {
        fprintf(stderr, "Error: Attempt to free item %u from non-existent segment %s%d\n", global_num, prefix, segment_index);
        return;
    }

    FILE *fp = get_segment_fp(prefix, segment_index, "r+b", false);
    if (!fp)
    {
        fprintf(stderr, "Error opening segment %s%d to free item %u\n", prefix, segment_index, global_num);
        return;
    }

    char bitmap_block[BLOCK_SIZE];
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE)
    {
        perror("Error reading bitmap block for freeing");
        fclose(fp);
        return;
    }

    if (!is_bit_set(bitmap_block, bit_index_in_segment))
    {
        fprintf(stderr, "Warning: Attempting to free already free item %u in %s%d\n", global_num, prefix, segment_index);
    }

    clear_bit(bitmap_block, bit_index_in_segment);
    // printf("free_generic: Freed %s number %u from segment %d, bit %d\n", prefix, global_num, segment_index, bit_index_in_segment);

    if (fseek(fp, 0, SEEK_SET) != 0 || fwrite(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE)
    {
        perror("Error writing updated bitmap block after freeing");
    }
    fclose(fp);
}

void free_inode(uint32_t inode_num)
{
    free_generic(inode_num, INODE_SEGMENT_PREFIX, USABLE_INODES_PER_SEGMENT);
}

void free_block(uint32_t block_num)
{
    free_generic(block_num, DATA_SEGMENT_PREFIX, USABLE_DATA_BLOCKS_PER_SEGMENT);
}

// --- Initialization ---
void initialize_exfs2()
{
    // printf("Initializing ExFS2 file system (or checking existing state)...\n");

    // Always scan for max indices first to know what exists
    int i = 0;
    char fname[256];
    int scanned_max_inode_idx = -1;
    while (true)
    {
        snprintf(fname, sizeof(fname), INODE_SEGMENT_PREFIX "%d" SEGMENT_SUFFIX, i);
        if (access(fname, F_OK) == 0)
            scanned_max_inode_idx = i;
        else
            break;
        i++;
    }
    i = 0;
    int scanned_max_data_idx = -1;
    while (true)
    {
        snprintf(fname, sizeof(fname), DATA_SEGMENT_PREFIX "%d" SEGMENT_SUFFIX, i);
        if (access(fname, F_OK) == 0)
            scanned_max_data_idx = i;
        else
            break;
        i++;
    }

    max_inode_segment_idx = scanned_max_inode_idx;
    max_data_segment_idx = scanned_max_data_idx;
    // printf("Scanned max indices: inode_seg=%d, data_seg=%d\n", max_inode_segment_idx, max_data_segment_idx);

    bool needs_full_init = (max_inode_segment_idx < 0 || max_data_segment_idx < 0);

    if (needs_full_init)
    {
        printf("Performing full ExFS2 initialization (segment files not found or incomplete).\n");

        // Ensure segment 0 files exist. get_segment_fp with create=true handles this.
        // It also updates max_inode_segment_idx and max_data_segment_idx if they were -1.
        FILE *fp_i = get_segment_fp(INODE_SEGMENT_PREFIX, 0, "r+b", true);
        if (!fp_i)
        {
            fprintf(stderr, "Failed to initialize/open inode segment 0\n");
            exit(EXIT_FAILURE);
        }
        fclose(fp_i);

        FILE *fp_d = get_segment_fp(DATA_SEGMENT_PREFIX, 0, "r+b", true);
        if (!fp_d)
        {
            fprintf(stderr, "Failed to initialize/open data segment 0\n");
            exit(EXIT_FAILURE);
        }
        fclose(fp_d);

        // max_inode_segment_idx and max_data_segment_idx should now be at least 0.

        // Initialize inode bitmap (segment 0, block 0)
        char inode_bitmap_block[BLOCK_SIZE] = {0};
        set_bit(inode_bitmap_block, ROOT_INODE_NUM);
        FILE *fp_i0_bitmap = get_segment_fp(INODE_SEGMENT_PREFIX, 0, "r+b", false);
        if (!fp_i0_bitmap)
        {
            perror("Failed open inode segment 0 for bitmap init");
            exit(EXIT_FAILURE);
        }
        if (fseek(fp_i0_bitmap, 0, SEEK_SET) != 0 || fwrite(inode_bitmap_block, 1, BLOCK_SIZE, fp_i0_bitmap) != BLOCK_SIZE)
        {
            perror("Init: write inode bitmap");
            fclose(fp_i0_bitmap);
            exit(EXIT_FAILURE);
        }
        fclose(fp_i0_bitmap);

        // Initialize data bitmap (segment 0, block 0)
        uint32_t root_data_block = 0;
        char data_bitmap_block[BLOCK_SIZE] = {0};
        set_bit(data_bitmap_block, root_data_block);
        FILE *fp_d0_bitmap = get_segment_fp(DATA_SEGMENT_PREFIX, 0, "r+b", false);
        if (!fp_d0_bitmap)
        {
            perror("Failed open data segment 0 for bitmap init");
            exit(EXIT_FAILURE);
        }
        if (fseek(fp_d0_bitmap, 0, SEEK_SET) != 0 || fwrite(data_bitmap_block, 1, BLOCK_SIZE, fp_d0_bitmap) != BLOCK_SIZE)
        {
            perror("Init: write data bitmap");
            fclose(fp_d0_bitmap);
            exit(EXIT_FAILURE);
        }
        fclose(fp_d0_bitmap);

        // Initialize root inode structure (inode 0)
        exfs2_inode root_inode_data = {0};
        root_inode_data.mode = EXFS2_IFDIR;
        root_inode_data.size = 0;
        root_inode_data.direct_blocks[0] = root_data_block;

        // Initialize root data block (data block 0) with "." and ".."
        char root_dir_block_buf[BLOCK_SIZE] = {0};
        exfs2_dirent *dir_entries = (exfs2_dirent *)root_dir_block_buf;
        dir_entries[0].inode_num = ROOT_INODE_NUM;
        strncpy(dir_entries[0].name, ".", MAX_FILENAME_LEN);
        dir_entries[0].name[MAX_FILENAME_LEN] = '\0';
        root_inode_data.size += sizeof(exfs2_dirent);
        dir_entries[1].inode_num = ROOT_INODE_NUM;
        strncpy(dir_entries[1].name, "..", MAX_FILENAME_LEN);
        dir_entries[1].name[MAX_FILENAME_LEN] = '\0';
        root_inode_data.size += sizeof(exfs2_dirent);

        if (write_block(root_data_block, root_dir_block_buf) != 0)
        {
            fprintf(stderr, "Initialization error: Failed write root data block\n");
            exit(EXIT_FAILURE);
        }
        if (write_inode(ROOT_INODE_NUM, &root_inode_data) != 0)
        {
            fprintf(stderr, "Initialization error: Failed write root inode\n");
            exit(EXIT_FAILURE);
        }
        printf("ExFS2 file system initialized successfully.\n");
    }
    else
    {
        // printf("ExFS2 file system appears to be initialized (segments found).\n");
        // printf("Max Inode Segment Index: %d\n", max_inode_segment_idx);
        // printf("Max Data Segment Index: %d\n", max_data_segment_idx);
    }
}

// --- Path Traversal ---
uint32_t find_entry_in_dir(uint32_t dir_inode_num, const char *name)
{
    exfs2_inode dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0)
        return 0;
    if (!(dir_inode.mode & EXFS2_IFDIR))
        return 0;

    char block_buf[BLOCK_SIZE];
    for (int i = 0; i < NUM_DIRECT; ++i)
    {
        uint32_t current_data_block = dir_inode.direct_blocks[i];
        if (current_data_block == 0)
            continue;

        if (read_block(current_data_block, block_buf) != 0)
        {
            fprintf(stderr, "Warning: Failed to read data block %u for dir inode %u in find_entry_in_dir\n", current_data_block, dir_inode_num);
            continue;
        }

        exfs2_dirent *entries = (exfs2_dirent *)block_buf;
        for (int j = 0; j < DIRENTS_PER_BLOCK; ++j)
        {
            if (entries[j].inode_num != 0 && strncmp(entries[j].name, name, MAX_FILENAME_LEN) == 0)
            {
                return entries[j].inode_num;
            }
        }
    }
    // TODO: Indirect blocks for find_entry_in_dir
    return 0;
}

// Traverses a path.
// If create_missing is true: it creates any missing directory components along the path.
// If last_component is not NULL: it will store the name of the *final component processed* from the path string.
// result_inode_num will contain the inode of the final component found or created.
// Returns 0 on success, -1 on error.
int traverse_path(const char *path, uint32_t *result_inode_num, bool create_missing, char *last_component_out)
{
    if (path == NULL)
    {
        fprintf(stderr, "traverse_path: NULL path provided.\n");
        return -1;
    }
    char *path_copy = strdup(path);
    if (!path_copy)
    {
        perror("strdup in traverse_path");
        return -1;
    }

    char *current_path_ptr = path_copy;
    while (current_path_ptr[0] == '/')
        current_path_ptr++; // Skip leading slashes

    uint32_t current_inode_num = ROOT_INODE_NUM;
    *result_inode_num = ROOT_INODE_NUM;

    if (strlen(current_path_ptr) == 0)
    { // Path was effectively "/" or empty
        if (last_component_out)
            strcpy(last_component_out, "/");
        free(path_copy);
        return 0;
    }

    char *token;
    char *rest = current_path_ptr;
    char component[MAX_FILENAME_LEN + 1];
    uint32_t parent_inode_num = ROOT_INODE_NUM;

    while ((token = strtok_r(rest, "/", &rest)))
    {
        strncpy(component, token, MAX_FILENAME_LEN);
        component[MAX_FILENAME_LEN] = '\0';

        exfs2_inode current_dir_inode_data;
        if (read_inode(current_inode_num, &current_dir_inode_data) != 0)
        {
            fprintf(stderr, "traverse_path: Failed read inode %u (for component '%s')\n", current_inode_num, component);
            free(path_copy);
            return -1;
        }
        if (!(current_dir_inode_data.mode & EXFS2_IFDIR))
        {
            fprintf(stderr, "traverse_path: Path component error - inode %u is not a directory (when trying to process '%s')\n", current_inode_num, component);
            free(path_copy);
            return -1;
        }

        parent_inode_num = current_inode_num;
        uint32_t next_inode_num = find_entry_in_dir(current_inode_num, component);

        if (next_inode_num == 0)
        { // Component Not Found
            if (create_missing)
            {
                // Create 'component' as a directory within 'parent_inode_num'
                // printf("traverse_path: Auto-creating directory component: '%s' in parent inode %u\n", component, parent_inode_num);

                uint32_t new_dir_inode_num = allocate_inode();
                if (new_dir_inode_num == UINT32_MAX)
                {
                    fprintf(stderr, "traverse_path: Failed to allocate inode for new directory '%s'\n", component);
                    free(path_copy);
                    return -1;
                }
                uint32_t new_dir_block_num = allocate_block();
                if (new_dir_block_num == 0)
                {
                    fprintf(stderr, "traverse_path: Failed to allocate data block for new directory '%s'\n", component);
                    free_inode(new_dir_inode_num);
                    free(path_copy);
                    return -1;
                }

                exfs2_inode new_dir_inode = {0};
                new_dir_inode.mode = EXFS2_IFDIR;
                new_dir_inode.direct_blocks[0] = new_dir_block_num;
                new_dir_inode.size = 0;

                char new_dir_block_buf[BLOCK_SIZE] = {0};
                exfs2_dirent *entries = (exfs2_dirent *)new_dir_block_buf;
                entries[0].inode_num = new_dir_inode_num;
                strncpy(entries[0].name, ".", MAX_FILENAME_LEN);
                entries[0].name[MAX_FILENAME_LEN] = '\0';
                new_dir_inode.size += sizeof(exfs2_dirent);
                entries[1].inode_num = parent_inode_num;
                strncpy(entries[1].name, "..", MAX_FILENAME_LEN);
                entries[1].name[MAX_FILENAME_LEN] = '\0';
                new_dir_inode.size += sizeof(exfs2_dirent);

                if (write_block(new_dir_block_num, new_dir_block_buf) != 0)
                {
                    fprintf(stderr, "traverse_path: Failed write data block %u for new dir '%s'\n", new_dir_block_num, component);
                    free_inode(new_dir_inode_num);
                    free_block(new_dir_block_num);
                    free(path_copy);
                    return -1;
                }
                if (write_inode(new_dir_inode_num, &new_dir_inode) != 0)
                {
                    fprintf(stderr, "traverse_path: Failed write inode %u for new dir '%s'\n", new_dir_inode_num, component);
                    free_inode(new_dir_inode_num);
                    free_block(new_dir_block_num);
                    free(path_copy);
                    return -1;
                }
                if (add_entry_to_dir(parent_inode_num, component, new_dir_inode_num) != 0)
                {
                    fprintf(stderr, "traverse_path: Failed add entry '%s' to parent dir inode %u\n", component, parent_inode_num);
                    recursive_free(new_dir_inode_num);
                    free(path_copy);
                    return -1;
                }
                next_inode_num = new_dir_inode_num;
            }
            else
            { // Not found, and not creating
                // fprintf(stderr, "Path not found: Component '%s' does not exist in directory inode %u\n", component, parent_inode_num);
                free(path_copy);
                return -1;
            }
        }
        current_inode_num = next_inode_num;

        if (rest == NULL || strlen(rest) == 0)
        { // This was the last component from strtok_r
            if (last_component_out)
            {
                strncpy(last_component_out, component, MAX_FILENAME_LEN + 1);
                last_component_out[MAX_FILENAME_LEN] = '\0';
            }
        }
    }
    *result_inode_num = current_inode_num;
    free(path_copy);
    return 0;
}

// --- Directory Operations ---
int add_entry_to_dir(uint32_t parent_inode_num, const char *name, uint32_t entry_inode_num)
{
    exfs2_inode parent_inode;
    if (read_inode(parent_inode_num, &parent_inode) != 0)
        return -1;
    if (!(parent_inode.mode & EXFS2_IFDIR))
        return -1;

    char block_buf[BLOCK_SIZE];
    for (int i = 0; i < NUM_DIRECT; ++i)
    {
        uint32_t block_num = parent_inode.direct_blocks[i];
        if (block_num != 0)
        { // Existing block
            if (read_block(block_num, block_buf) != 0)
                continue;
            exfs2_dirent *entries = (exfs2_dirent *)block_buf;
            for (int j = 0; j < DIRENTS_PER_BLOCK; ++j)
            {
                if (entries[j].inode_num == 0)
                {
                    entries[j].inode_num = entry_inode_num;
                    strncpy(entries[j].name, name, MAX_FILENAME_LEN);
                    entries[j].name[MAX_FILENAME_LEN] = '\0';
                    if (write_block(block_num, block_buf) != 0)
                        return -1;
                    parent_inode.size += sizeof(exfs2_dirent);
                    if (write_inode(parent_inode_num, &parent_inode) != 0)
                        return -1;
                    return 0;
                }
            }
        }
    }
    // No space in existing direct blocks, try allocating a new one
    for (int i = 0; i < NUM_DIRECT; ++i)
    {
        if (parent_inode.direct_blocks[i] == 0)
        {
            uint32_t new_block_num = allocate_block();
            if (new_block_num == 0)
                return -1;
            parent_inode.direct_blocks[i] = new_block_num;
            memset(block_buf, 0, BLOCK_SIZE);
            exfs2_dirent *entries = (exfs2_dirent *)block_buf;
            entries[0].inode_num = entry_inode_num;
            strncpy(entries[0].name, name, MAX_FILENAME_LEN);
            entries[0].name[MAX_FILENAME_LEN] = '\0';
            if (write_block(new_block_num, block_buf) != 0)
            {
                free_block(new_block_num);
                parent_inode.direct_blocks[i] = 0;
                return -1;
            }
            parent_inode.size += sizeof(exfs2_dirent);
            if (write_inode(parent_inode_num, &parent_inode) != 0)
            {
                // Tricky: block written, entry made, inode update failed.
                // For now, report error. Could try to revert entry.
                return -1;
            }
            return 0;
        }
    }
    // TODO: Indirect blocks for add_entry_to_dir
    fprintf(stderr, "add_entry_to_dir: Directory inode %u full (direct blocks).\n", parent_inode_num);
    return -1;
}

int remove_entry_from_dir(uint32_t parent_inode_num, const char *name)
{
    exfs2_inode parent_inode;
    if (read_inode(parent_inode_num, &parent_inode) != 0)
        return -1;
    if (!(parent_inode.mode & EXFS2_IFDIR))
        return -1;
    char block_buf[BLOCK_SIZE];
    bool found = false;
    for (int i = 0; i < NUM_DIRECT && parent_inode.direct_blocks[i] != 0; ++i)
    {
        uint32_t block_num = parent_inode.direct_blocks[i];
        if (read_block(block_num, block_buf) != 0)
            continue;
        exfs2_dirent *entries = (exfs2_dirent *)block_buf;
        for (int j = 0; j < DIRENTS_PER_BLOCK; ++j)
        {
            if (entries[j].inode_num != 0 && strcmp(entries[j].name, name) == 0)
            {
                entries[j].inode_num = 0;
                memset(entries[j].name, 0, MAX_FILENAME_LEN + 1);
                found = true;
                if (write_block(block_num, block_buf) != 0)
                    return -1;
                // Not updating parent_inode.size here for simplicity.
                // TODO: Consider if block should be freed if all entries become 0.
                goto end_search;
            }
        }
    }
end_search:
    if (!found)
    {
        fprintf(stderr, "remove_entry_from_dir: Entry '%s' not found in dir inode %u.\n", name, parent_inode_num);
        return -1;
    }
    return 0;
}

// --- Command Implementations ---
// Added current_path_prefix for more informative listing
void list_directory(uint32_t dir_inode_num, int indent, const char *current_path_prefix)
{
    exfs2_inode dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0)
    {
        fprintf(stderr, "ls: Failed to read inode %u for path prefix '%s'\n", dir_inode_num, current_path_prefix);
        return;
    }
    if (!(dir_inode.mode & EXFS2_IFDIR))
    {
        // This case should ideally be caught by the caller of list_directory
        // fprintf(stderr, "ls: '%s' (inode %u) is not a directory.\n", current_path_prefix, dir_inode_num);
        return;
    }

    // printf("Listing directory inode %u ('%s'), indent %d\n", dir_inode_num, current_path_prefix, indent);

    char block_buf[BLOCK_SIZE];
    char entry_full_path[2048];

    for (int i = 0; i < NUM_DIRECT; ++i)
    {
        uint32_t block_num = dir_inode.direct_blocks[i];
        if (block_num == 0)
            continue;

        if (read_block(block_num, block_buf) != 0)
        {
            fprintf(stderr, "Warning: ls: Failed to read data block %u for dir inode %u ('%s')\n", block_num, dir_inode_num, current_path_prefix);
            continue;
        }

        exfs2_dirent *entries = (exfs2_dirent *)block_buf;
        for (int j = 0; j < DIRENTS_PER_BLOCK; ++j)
        {
            if (entries[j].inode_num != 0)
            {
                // Construct full path for display and recursion
                if (strcmp(current_path_prefix, "/") == 0)
                {
                    snprintf(entry_full_path, sizeof(entry_full_path), "/%s", entries[j].name);
                }
                else
                {
                    snprintf(entry_full_path, sizeof(entry_full_path), "%s/%s", current_path_prefix, entries[j].name);
                }

                for (int k = 0; k < indent; ++k)
                    printf("  ");

                exfs2_inode entry_inode;
                bool is_dir = false;
                const char *suffix = "";
                if (read_inode(entries[j].inode_num, &entry_inode) == 0)
                {
                    if (entry_inode.mode & EXFS2_IFDIR)
                    {
                        is_dir = true;
                        suffix = "/";
                    }
                }
                else
                {
                    fprintf(stderr, "\nWarning: ls: Couldn't read inode %u ('%s') listed in dir %u ('%s')\n", entries[j].inode_num, entries[j].name, dir_inode_num, current_path_prefix);
                    suffix = " (inode read error)";
                }
                printf("%s%s\n", entries[j].name, suffix); // Print just the name, not full path here

                if (is_dir && strcmp(entries[j].name, ".") != 0 && strcmp(entries[j].name, "..") != 0)
                {
                    list_directory(entries[j].inode_num, indent + 1, entry_full_path);
                }
            }
        }
    }
    // TODO: Indirect blocks for list_directory
}

uint32_t get_block_num_for_file_offset(exfs2_inode *inode, uint64_t offset, bool allocate_if_needed, uint32_t file_inode_num)
{
    if (!inode)
        return 0;
    uint64_t block_index_in_file = offset / BLOCK_SIZE;
    uint32_t block_num = 0;

    if (block_index_in_file < NUM_DIRECT)
    {
        block_num = inode->direct_blocks[block_index_in_file];
        if (block_num == 0 && allocate_if_needed)
        {
            block_num = allocate_block();
            if (block_num == 0)
            {
                fprintf(stderr, "get_block_num: Failed alloc direct block %" PRIu64 " for inode %u\n", block_index_in_file, file_inode_num);
                return 0;
            }
            inode->direct_blocks[block_index_in_file] = block_num;
            if (write_inode(file_inode_num, inode) != 0)
            {
                fprintf(stderr, "get_block_num: Failed write inode %u after alloc direct block\n", file_inode_num);
                free_block(block_num);
                inode->direct_blocks[block_index_in_file] = 0;
                return 0;
            }
        }
        return block_num;
    }

    uint64_t direct_limit = NUM_DIRECT;
    uint64_t single_limit = direct_limit + POINTERS_PER_INDIRECT_BLOCK;
    if (block_index_in_file < single_limit)
    {
        if (inode->single_indirect == 0)
        {
            if (!allocate_if_needed)
                return 0;
            inode->single_indirect = allocate_block();
            if (inode->single_indirect == 0)
            {
                fprintf(stderr, "get_block_num: Failed alloc single indirect block for inode %u\n", file_inode_num);
                return 0;
            }
            if (write_inode(file_inode_num, inode) != 0)
            {
                fprintf(stderr, "get_block_num: Failed write inode %u after alloc single indirect block\n", file_inode_num);
                free_block(inode->single_indirect);
                inode->single_indirect = 0;
                return 0;
            }
            char zero_buf[BLOCK_SIZE] = {0};
            if (write_block(inode->single_indirect, zero_buf) != 0)
            { /* Handle error */
            }
        }
        indirect_block indir_block;
        if (read_block(inode->single_indirect, (char *)&indir_block) != 0)
            return 0;
        uint32_t index_in_indirect = block_index_in_file - direct_limit;
        block_num = indir_block.block_ptrs[index_in_indirect];
        if (block_num == 0 && allocate_if_needed)
        {
            block_num = allocate_block();
            if (block_num == 0)
                return 0;
            indir_block.block_ptrs[index_in_indirect] = block_num;
            if (write_block(inode->single_indirect, (char *)&indir_block) != 0)
            {
                free_block(block_num);
                indir_block.block_ptrs[index_in_indirect] = 0;
                return 0;
            }
        }
        return block_num;
    }
    // TODO: Double/Triple Indirect
    fprintf(stderr, "Offset %" PRIu64 " too large (Double/Triple indirect not implemented).\n", offset);
    return 0;
}

void add_file(const char *exfs2_path, const char *local_path)
{
    char parent_path_buf[1024];
    char filename_buf[MAX_FILENAME_LEN + 1];
    uint32_t parent_inode_num;

    char *path_copy_for_dirname = strdup(exfs2_path);
    if (!path_copy_for_dirname)
    {
        perror("strdup for dirname in add_file");
        return;
    }
    char *temp_parent_path = dirname(path_copy_for_dirname);
    strncpy(parent_path_buf, temp_parent_path, sizeof(parent_path_buf) - 1);
    parent_path_buf[sizeof(parent_path_buf) - 1] = '\0';

    char *path_copy_for_basename = strdup(exfs2_path);
    if (!path_copy_for_basename)
    {
        perror("strdup for basename in add_file");
        free(path_copy_for_dirname);
        return;
    }
    char *temp_filename = basename(path_copy_for_basename);
    strncpy(filename_buf, temp_filename, MAX_FILENAME_LEN);
    filename_buf[MAX_FILENAME_LEN] = '\0';

    // printf("add_file: exfs2_path='%s', local_path='%s'\n", exfs2_path, local_path);
    // printf("add_file: Derived parent_path_buf='%s', filename_buf='%s'\n", parent_path_buf, filename_buf);

    free(path_copy_for_dirname);
    free(path_copy_for_basename);

    if (traverse_path(parent_path_buf, &parent_inode_num, true, NULL) != 0)
    {
        fprintf(stderr, "add_file: Failed to traverse/create parent path '%s'\n", parent_path_buf);
        return;
    }
    // printf("add_file: Parent directory '%s' is inode %u\n", parent_path_buf, parent_inode_num);

    if (find_entry_in_dir(parent_inode_num, filename_buf) != 0)
    {
        fprintf(stderr, "add_file: Error - File '%s' already exists in '%s' (inode %u)\n", filename_buf, parent_path_buf, parent_inode_num);
        return;
    }

    FILE *local_fp = fopen(local_path, "rb");
    if (!local_fp)
    {
        perror("add_file: Failed to open local file");
        fprintf(stderr, "  Path: %s\n", local_path);
        return;
    }

    uint32_t new_file_inode_num = allocate_inode();
    if (new_file_inode_num == UINT32_MAX)
    {
        fclose(local_fp);
        return;
    }

    exfs2_inode new_file_inode = {0};
    new_file_inode.mode = EXFS2_IFREG;
    new_file_inode.size = 0;
    char buffer[BLOCK_SIZE];
    size_t bytes_read;
    uint64_t current_offset = 0;
    bool write_error = false;

    while ((bytes_read = fread(buffer, 1, BLOCK_SIZE, local_fp)) > 0)
    {
        uint32_t target_block_num = get_block_num_for_file_offset(&new_file_inode, current_offset, true, new_file_inode_num);
        if (target_block_num == 0)
        {
            write_error = true;
            break;
        }
        if (bytes_read < BLOCK_SIZE)
            memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
        if (write_block(target_block_num, buffer) != 0)
        {
            write_error = true;
            break;
        }
        new_file_inode.size += bytes_read;
        current_offset += bytes_read;
    }
    if (ferror(local_fp))
    {
        perror("add_file: Error reading local file");
        write_error = true;
    }
    fclose(local_fp);

    if (write_error)
    {
        recursive_free(new_file_inode_num);
        return;
    }
    if (write_inode(new_file_inode_num, &new_file_inode) != 0)
    {
        recursive_free(new_file_inode_num);
        return;
    }
    if (add_entry_to_dir(parent_inode_num, filename_buf, new_file_inode_num) != 0)
    {
        recursive_free(new_file_inode_num);
        return;
    }
    printf("Added '%s' to ExFS2 as '%s' (inode %u, size %" PRIu64 " bytes)\n", local_path, exfs2_path, new_file_inode_num, new_file_inode.size);
}

void extract_file(const char *exfs2_path)
{
    uint32_t file_inode_num;
    char last_comp[MAX_FILENAME_LEN + 1];
    if (traverse_path(exfs2_path, &file_inode_num, false, last_comp) != 0)
    {
        fprintf(stderr, "cat: Cannot access '%s': No such file or directory\n", exfs2_path);
        return;
    }
    exfs2_inode file_inode;
    if (read_inode(file_inode_num, &file_inode) != 0)
        return;
    if (!(file_inode.mode & EXFS2_IFREG))
    {
        fprintf(stderr, "cat: '%s' is not a regular file.\n", exfs2_path);
        return;
    }
    char buffer[BLOCK_SIZE];
    uint64_t remaining_size = file_inode.size;
    uint64_t current_offset = 0;
    while (remaining_size > 0)
    {
        uint32_t block_num = get_block_num_for_file_offset(&file_inode, current_offset, false, file_inode_num);
        if (block_num == 0)
        {
            fprintf(stderr, "cat: Error reading '%s' at offset %" PRIu64 "\n", exfs2_path, current_offset);
            break;
        }
        if (read_block(block_num, buffer) != 0)
        {
            fprintf(stderr, "cat: Error reading block %u for '%s'\n", block_num, exfs2_path);
            break;
        }
        size_t bytes_to_write = (remaining_size < BLOCK_SIZE) ? (size_t)remaining_size : BLOCK_SIZE;
        if (fwrite(buffer, 1, bytes_to_write, stdout) != bytes_to_write)
        {
            perror("cat: Error writing to stdout");
            break;
        }
        remaining_size -= bytes_to_write;
        current_offset += bytes_to_write;
    }
    fflush(stdout);
}

void recursive_free(uint32_t inode_num)
{
    if (inode_num == 0 || inode_num == UINT32_MAX)
        return;
    exfs2_inode inode;
    if (read_inode(inode_num, &inode) != 0)
    {
        free_inode(inode_num);
        return;
    }

    if (inode.mode & EXFS2_IFDIR)
    {
        char block_buf[BLOCK_SIZE];
        for (int i = 0; i < NUM_DIRECT; ++i)
        {
            uint32_t block_num = inode.direct_blocks[i];
            if (block_num == 0)
                continue;
            if (read_block(block_num, block_buf) == 0)
            {
                exfs2_dirent *entries = (exfs2_dirent *)block_buf;
                for (int j = 0; j < DIRENTS_PER_BLOCK; ++j)
                {
                    if (entries[j].inode_num != 0 && strcmp(entries[j].name, ".") != 0 && strcmp(entries[j].name, "..") != 0)
                    {
                        recursive_free(entries[j].inode_num);
                    }
                }
            }
        }
        // TODO: Indirect blocks for directory contents
    }
    for (int i = 0; i < NUM_DIRECT; ++i)
        if (inode.direct_blocks[i] != 0)
            free_block(inode.direct_blocks[i]);
    if (inode.single_indirect != 0)
    {
        indirect_block indir;
        if (read_block(inode.single_indirect, (char *)&indir) == 0)
        {
            for (int i = 0; i < POINTERS_PER_INDIRECT_BLOCK; ++i)
                if (indir.block_ptrs[i] != 0)
                    free_block(indir.block_ptrs[i]);
        }
        free_block(inode.single_indirect);
    }
    // TODO: Free double/triple
    free_inode(inode_num);
}

void remove_file_or_dir(const char *exfs2_path)
{
    if (strcmp(exfs2_path, "/") == 0)
    {
        fprintf(stderr, "rm: Cannot remove '/'\n");
        return;
    }
    char parent_path_buf[1024], target_name_buf[MAX_FILENAME_LEN + 1];
    uint32_t parent_inode_num, target_inode_num;

    char *dirc = strdup(exfs2_path);
    if (!dirc)
        return;
    char *basec = strdup(exfs2_path);
    if (!basec)
    {
        free(dirc);
        return;
    }
    strncpy(parent_path_buf, dirname(dirc), sizeof(parent_path_buf) - 1);
    parent_path_buf[sizeof(parent_path_buf) - 1] = 0;
    strncpy(target_name_buf, basename(basec), MAX_FILENAME_LEN);
    target_name_buf[MAX_FILENAME_LEN] = 0;
    free(dirc);
    free(basec);

    if (strcmp(target_name_buf, ".") == 0 || strcmp(target_name_buf, "..") == 0)
    {
        fprintf(stderr, "rm: Cannot remove '.' or '..'\n");
        return;
    }
    if (traverse_path(parent_path_buf, &parent_inode_num, false, NULL) != 0)
    {
        fprintf(stderr, "rm: Cannot access parent of '%s'\n", exfs2_path);
        return;
    }
    target_inode_num = find_entry_in_dir(parent_inode_num, target_name_buf);
    if (target_inode_num == 0)
    {
        fprintf(stderr, "rm: Cannot remove '%s': No such file or directory\n", exfs2_path);
        return;
    }
    if (remove_entry_from_dir(parent_inode_num, target_name_buf) != 0)
        return;
    recursive_free(target_inode_num);
    printf("Removed '%s'\n", exfs2_path);
}

void debug_path(const char *exfs2_path)
{
    char *path_copy = strdup(exfs2_path);
    if (!path_copy)
    {
        perror("debug_path strdup");
        return;
    }
    char *current_path_ptr = path_copy;
    uint32_t current_inode_num = ROOT_INODE_NUM;
    printf("Debugging path resolution for: '%s'\n", exfs2_path);

    exfs2_inode current_inode_data;
    if (read_inode(ROOT_INODE_NUM, &current_inode_data) == 0)
    {
        printf("Inode %u (ROOT): mode=%s, size=%" PRIu64 "\n", ROOT_INODE_NUM,
               (current_inode_data.mode & EXFS2_IFDIR) ? "DIR" : "REG", current_inode_data.size);
        printf("  Direct blocks: ");
        for (int k = 0; k < NUM_DIRECT; ++k)
            printf("[%d]=%u ", k, current_inode_data.direct_blocks[k]);
        printf("\n  Indirect: S=%u D=%u T=%u\n", current_inode_data.single_indirect, current_inode_data.double_indirect, current_inode_data.triple_indirect);
    }
    else
    {
        free(path_copy);
        return;
    }

    while (current_path_ptr[0] == '/')
        current_path_ptr++;
    if (strlen(current_path_ptr) == 0)
    {
        printf("Path resolves to ROOT inode %u\n", ROOT_INODE_NUM);
        free(path_copy);
        return;
    }

    char *token, *rest = current_path_ptr, component[MAX_FILENAME_LEN + 1];
    while ((token = strtok_r(rest, "/", &rest)))
    {
        strncpy(component, token, MAX_FILENAME_LEN);
        component[MAX_FILENAME_LEN] = '\0';
        printf("--> Searching for: '%s' in dir inode %u\n", component, current_inode_num);

        if (read_inode(current_inode_num, &current_inode_data) != 0 || !(current_inode_data.mode & EXFS2_IFDIR))
        {
            fprintf(stderr, "Debug: Error - inode %u not a directory or unreadable.\n", current_inode_num);
            break;
        }
        uint32_t next_inode_num = find_entry_in_dir(current_inode_num, component);
        if (next_inode_num == 0)
        {
            printf("  Component '%s' NOT FOUND\n", component);
            break;
        }

        current_inode_num = next_inode_num;
        if (read_inode(current_inode_num, &current_inode_data) == 0)
        {
            printf("Inode %u ('%s'): mode=%s, size=%" PRIu64 "\n", current_inode_num, component,
                   (current_inode_data.mode & EXFS2_IFDIR) ? "DIR" : "REG", current_inode_data.size);
            printf("  Direct blocks: ");
            for (int k = 0; k < NUM_DIRECT; ++k)
                printf("[%d]=%u ", k, current_inode_data.direct_blocks[k]);
            printf("\n  Indirect: S=%u D=%u T=%u\n", current_inode_data.single_indirect, current_inode_data.double_indirect, current_inode_data.triple_indirect);
        }
        else
        {
            fprintf(stderr, "Debug: Failed read inode %u for '%s'\n", current_inode_num, component);
        }
        if (rest == NULL || strlen(rest) == 0)
        {
            printf("Path resolution finished at inode %u ('%s')\n", current_inode_num, component);
            break;
        }
    }
    free(path_copy);
}

// --- Main Function ---
void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s <operation> [arguments...]\n", prog_name);
    fprintf(stderr, "Operations:\n");
    fprintf(stderr, "  --ls <exfs2_path>           List directory contents recursively\n");
    fprintf(stderr, "  --add <local_path> <exfs2_path> Add a local file to the filesystem\n");
    fprintf(stderr, "  --cat <exfs2_path>          Extract file contents to standard output\n");
    fprintf(stderr, "  --rm <exfs2_path>           Remove a file or directory recursively\n");
    fprintf(stderr, "  --debug <exfs2_path>        Debug path traversal and inode info\n");
}

int main(int argc, char *argv[])
{
    // Automatic initialization check
    initialize_exfs2();

    if (argc < 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    int opt = 0;
    int long_index = 0;
    const char *op_ls_path = NULL;
    const char *op_add_local = NULL;
    const char *op_add_exfs2 = NULL;
    const char *op_cat_path = NULL;
    const char *op_rm_path = NULL;
    const char *op_debug_path = NULL;

    static struct option long_options[] = {
        {"ls", required_argument, 0, 'l'},
        {"add", required_argument, 0, 'a'},
        {"cat", required_argument, 0, 'c'},
        {"rm", required_argument, 0, 'r'},
        {"debug", required_argument, 0, 'd'},
        {0, 0, 0, 0}};

    // Use "+" at start of optstring to stop parsing on first non-option arg
    // if we don't want permutation. However, for --add L R, we need permutation or careful arg handling.
    // The current structure processes one long_option and then breaks.
    while ((opt = getopt_long(argc, argv, "+l:a:c:r:d:", long_options, &long_index)) != -1)
    {
        switch (opt)
        {
        case 'l':
            op_ls_path = optarg;
            break;
        case 'a':
            op_add_local = optarg;
            if (optind < argc)
            {
                op_add_exfs2 = argv[optind];
                optind++;
            }
            else
            {
                fprintf(stderr, "Error: --add requires <local_path> <exfs2_path>\n");
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'c':
            op_cat_path = optarg;
            break;
        case 'r':
            op_rm_path = optarg;
            break;
        case 'd':
            op_debug_path = optarg;
            break;
        case '?':
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (opt != '?')
            break; // Process only the first operation
    }

    int ops_count = (op_ls_path ? 1 : 0) + (op_add_local ? 1 : 0) + (op_cat_path ? 1 : 0) + (op_rm_path ? 1 : 0) + (op_debug_path ? 1 : 0);
    if (ops_count == 0)
    {
        if (optind < argc)
            fprintf(stderr, "Error: Unknown operation or extra arguments: %s\n", argv[optind]);
        else
            fprintf(stderr, "Error: No operation specified.\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    // ops_count > 1 check is implicitly handled by breaking after first op.

    if (op_ls_path)
    {
        // printf("Listing directory: %s\n", op_ls_path);
        uint32_t target_inode;
        char last_comp[MAX_FILENAME_LEN + 1]; // Not strictly needed by ls if path must fully resolve
        if (traverse_path(op_ls_path, &target_inode, false, last_comp) == 0)
        {
            exfs2_inode target_inode_data;
            if (read_inode(target_inode, &target_inode_data) == 0)
            {
                if (target_inode_data.mode & EXFS2_IFDIR)
                {
                    // For root, print its name explicitly, otherwise list_directory handles sub-names
                    if (target_inode == ROOT_INODE_NUM && strcmp(op_ls_path, "/") == 0)
                    {
                        printf("/\n");
                        list_directory(target_inode, 1, "/"); // Start indent 1 for contents of root
                    }
                    else if (target_inode == ROOT_INODE_NUM)
                    { // e.g. ls . when CWD is conceptually /
                        list_directory(target_inode, 0, op_ls_path);
                    }
                    else
                    {
                        // For other dirs, print its name if it's not just "/"
                        // The list_directory will handle relative printing.
                        // We need to adjust how path prefix is passed or handled.
                        // For now, let's assume op_ls_path is the path to list.
                        // If it's a directory, list its contents.
                        // If op_ls_path is "/", list_directory starts with indent 0 or 1.
                        // If op_ls_path is "/a", list_directory starts with indent 0.
                        list_directory(target_inode, 0, op_ls_path);
                    }
                }
                else
                { // It's a file, just print its name (the last component)
                    printf("%s\n", last_comp);
                }
            }
            else
            {
                fprintf(stderr, "ls: Failed to read inode %u for '%s'\n", target_inode, op_ls_path);
                return EXIT_FAILURE;
            }
        }
        else
        {
            fprintf(stderr, "ls: Cannot access '%s': No such file or directory\n", op_ls_path);
            return EXIT_FAILURE;
        }
    }
    else if (op_add_local && op_add_exfs2)
    {
        add_file(op_add_exfs2, op_add_local);
    }
    else if (op_cat_path)
    {
        extract_file(op_cat_path);
    }
    else if (op_rm_path)
    {
        remove_file_or_dir(op_rm_path);
    }
    else if (op_debug_path)
    {
        debug_path(op_debug_path);
    }

    return EXIT_SUCCESS;
}