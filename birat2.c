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
#include <libgen.h> // For basename and dirname
#include <limits.h> // For UINT32_MAX
#include <inttypes.h> // For PRIu64 macro

// --- Constants ---
#define BLOCK_SIZE 4096
#define SEGMENT_SIZE (1 * 1024 * 1024) // 1MB
#define MAX_FILENAME_LEN 255
#define INODE_SEGMENT_PREFIX "inode_"
#define DATA_SEGMENT_PREFIX "data_"
#define SEGMENT_SUFFIX ".seg"

#define BITS_PER_BYTE 8
#define BLOCKS_PER_SEGMENT (SEGMENT_SIZE / BLOCK_SIZE) // 256
#define BITMAP_BLOCKS_PER_SEGMENT 1 // Reserve 1 block for bitmap
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
typedef struct {
    uint16_t mode;         // File type (EXFS2_IFREG, EXFS2_IFDIR)
    // uint32_t link_count;   // Add if needed for removal logic robustness
    uint64_t size;         // File size in bytes
    uint32_t direct_blocks[12]; // Placeholder - RECALCULATE BELOW
    uint32_t single_indirect; // Block number of the single indirect block (0 if unused)
    uint32_t double_indirect; // Block number of the double indirect block (0 if unused)
    uint32_t triple_indirect; // Block number of the triple indirect block (0 if unused)
    // Add padding if needed to exactly fill BLOCK_SIZE
} exfs2_inode_base; // Base structure for size calculation

#define INODE_METADATA_SIZE (sizeof(uint16_t) + sizeof(uint64_t) + 3 * sizeof(uint32_t))
#define INODE_DIRECT_POINTER_SPACE (BLOCK_SIZE - INODE_METADATA_SIZE)
#define NUM_DIRECT (INODE_DIRECT_POINTER_SPACE / sizeof(uint32_t))

#define POINTERS_PER_INDIRECT_BLOCK (BLOCK_SIZE / sizeof(uint32_t)) // 1024

// Final Inode Structure
typedef struct {
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
typedef struct {
    uint32_t inode_num;     // Inode number (0 if entry is unused)
    char name[MAX_FILENAME_LEN + 1]; // Null-terminated filename
    // Add type or length fields if implementing variable length entries
} exfs2_dirent;

#define DIRENTS_PER_BLOCK (BLOCK_SIZE / sizeof(exfs2_dirent))

// Indirect Block Structure
typedef struct {
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
void list_directory(uint32_t dir_inode_num, int indent);
void add_file(const char *exfs2_path, const char *local_path);
void extract_file(const char *exfs2_path);
void remove_file_or_dir(const char *exfs2_path);
void debug_path(const char* exfs2_path);
uint32_t find_entry_in_dir(uint32_t dir_inode_num, const char *name);
uint32_t get_block_num_for_file_offset(exfs2_inode *inode, uint64_t offset, bool allocate_if_needed, uint32_t file_inode_num);
// --- Added Forward Declarations ---
int add_entry_to_dir(uint32_t parent_inode_num, const char *name, uint32_t entry_inode_num);
void recursive_free(uint32_t inode_num);
// ---


// --- Utility Functions ---

void print_error(const char *msg) {
    fprintf(stderr, "Error: %s", msg);
    if (errno) {
        fprintf(stderr, " (%s)", strerror(errno));
    }
    fprintf(stderr, "\n");
}

// Gets the FILE* for a segment, creating the segment file if needed
FILE *get_segment_fp(const char *prefix, int index, const char *mode, bool create_if_missing) {
    char filename[256];
    snprintf(filename, sizeof(filename), "%s%d%s", prefix, index, SEGMENT_SUFFIX);

    FILE *fp = fopen(filename, mode);
    if (fp == NULL) {
        if (errno == ENOENT && create_if_missing) {
            // Create the file, fill with zeros
            fp = fopen(filename, "wb"); // Write binary
            if (fp == NULL) {
                perror("Error creating segment file");
                return NULL;
            }
            char zero_buffer[1024] = {0};
            for (int i = 0; i < SEGMENT_SIZE / sizeof(zero_buffer); ++i) {
                if (fwrite(zero_buffer, 1, sizeof(zero_buffer), fp) != sizeof(zero_buffer)) {
                    perror("Error writing zeros to new segment file");
                    fclose(fp);
                    return NULL;
                }
            }
            fclose(fp);

            // Reopen with the requested mode
            fp = fopen(filename, mode);
             if (fp == NULL) {
                 perror("Error reopening newly created segment file");
                 return NULL;
             }
             // Update max segment index tracking
            if (strcmp(prefix, INODE_SEGMENT_PREFIX) == 0 && index > max_inode_segment_idx) {
                max_inode_segment_idx = index;
            } else if (strcmp(prefix, DATA_SEGMENT_PREFIX) == 0 && index > max_data_segment_idx) {
                max_data_segment_idx = index;
            }

        } else {
            // Don't print error if just checking existence (e.g., "rb" mode)
            if (!(mode[0] == 'r' && mode[1] == 'b' && !create_if_missing)) {
                 perror("Error opening segment file");
            }
            return NULL;
        }
    }
    return fp;
}

// --- Bitmap Operations ---

// NOTE: These operate on a single block buffer assumed to be the bitmap block
void set_bit(char *bitmap, int bit_index) {
    int byte_index = bit_index / BITS_PER_BYTE;
    int bit_offset = bit_index % BITS_PER_BYTE;
    // Correct bounds check: compares bit index against total usable bits
    if (bit_index >= USABLE_BLOCKS_PER_SEGMENT) return; // Bounds check
    bitmap[byte_index] |= (1 << bit_offset);
}

void clear_bit(char *bitmap, int bit_index) {
    int byte_index = bit_index / BITS_PER_BYTE;
    int bit_offset = bit_index % BITS_PER_BYTE;
    // Correct bounds check: compares bit index against total usable bits
    if (bit_index >= USABLE_BLOCKS_PER_SEGMENT) return; // Bounds check
    bitmap[byte_index] &= ~(1 << bit_offset);
}

bool is_bit_set(const char *bitmap, int bit_index) {
    int byte_index = bit_index / BITS_PER_BYTE;
    int bit_offset = bit_index % BITS_PER_BYTE;
    // Correct bounds check: compares bit index against total usable bits
    if (bit_index >= USABLE_BLOCKS_PER_SEGMENT) return false; // Bounds check, treat out-of-bounds as not set
    return (bitmap[byte_index] & (1 << bit_offset)) != 0;
}


// Finds the first free bit (0) in the bitmap block buffer
// Returns bit index (0 to USABLE_BLOCKS_PER_SEGMENT-1) or -1 if full
int find_free_bit(const char *bitmap, int num_usable_items) {
    for (int i = 0; i < num_usable_items; ++i) {
        if (!is_bit_set(bitmap, i)) {
            return i; // Found a free bit
        }
    }
    return -1; // No free bits found
}

// --- Core Read/Write Operations ---

// Reads a block from the appropriate segment file
// block_num is the GLOBAL block number
int read_block(uint32_t block_num, char *buffer) {
     // Block 0 holds root directory data - reading it is valid

    int segment_index = block_num / USABLE_DATA_BLOCKS_PER_SEGMENT;
    int item_index_in_segment = block_num % USABLE_DATA_BLOCKS_PER_SEGMENT;
    // Data blocks start after the bitmap block(s)
    int block_offset_in_segment = item_index_in_segment + BITMAP_BLOCKS_PER_SEGMENT;
    off_t offset = (off_t)block_offset_in_segment * BLOCK_SIZE;

    FILE *fp = get_segment_fp(DATA_SEGMENT_PREFIX, segment_index, "rb", false); // Read binary
    if (!fp) return -1;

    if (fseek(fp, offset, SEEK_SET) != 0) {
        perror("Error seeking in data segment");
        fclose(fp);
        return -1;
    }
    size_t bytes_read = fread(buffer, 1, BLOCK_SIZE, fp);
    if (bytes_read != BLOCK_SIZE) {
        if (feof(fp)) {
             // Reduce noise: comment out warning unless debugging EOF issues
             // fprintf(stderr, "Warning: Premature EOF reading block %u (read %zu bytes) - filling remainder with zeros\n", block_num, bytes_read);
             memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read); // Zero out the rest
        } else {
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
int write_block(uint32_t block_num, const char *buffer) {
    // Block 0 holds root directory data - writing it is valid

    int segment_index = block_num / USABLE_DATA_BLOCKS_PER_SEGMENT;
    int item_index_in_segment = block_num % USABLE_DATA_BLOCKS_PER_SEGMENT;
    // Data blocks start after the bitmap block(s)
    int block_offset_in_segment = item_index_in_segment + BITMAP_BLOCKS_PER_SEGMENT;
    off_t offset = (off_t)block_offset_in_segment * BLOCK_SIZE;

    // Use "r+b" to modify existing file without truncating
    FILE *fp = get_segment_fp(DATA_SEGMENT_PREFIX, segment_index, "r+b", true); // Read/Write binary
    if (!fp) return -1;

    if (fseek(fp, offset, SEEK_SET) != 0) {
        perror("Error seeking in data segment for write");
        fclose(fp);
        return -1;
    }
    if (fwrite(buffer, 1, BLOCK_SIZE, fp) != BLOCK_SIZE) {
        perror("Error writing block to data segment");
        fclose(fp);
        return -1;
    }
    // Optimization: Consider removing fclose/fopen for consecutive writes to same segment
    fclose(fp);
    return 0;
}


// Reads an inode from the appropriate segment file
int read_inode(uint32_t inode_num, exfs2_inode *inode_buf) {
    int segment_index = inode_num / USABLE_INODES_PER_SEGMENT;
    int item_index_in_segment = inode_num % USABLE_INODES_PER_SEGMENT;
    // Inodes start after the bitmap block(s)
    int block_offset_in_segment = item_index_in_segment + BITMAP_BLOCKS_PER_SEGMENT;
    off_t offset = (off_t)block_offset_in_segment * BLOCK_SIZE;

    FILE *fp = get_segment_fp(INODE_SEGMENT_PREFIX, segment_index, "rb", false); // Read binary
    if (!fp) return -1;

    if (fseek(fp, offset, SEEK_SET) != 0) {
        perror("Error seeking in inode segment");
        fclose(fp);
        return -1;
    }
    size_t bytes_read = fread(inode_buf, 1, sizeof(exfs2_inode), fp);
    if (bytes_read != sizeof(exfs2_inode)) {
         if (feof(fp)) {
             // Reduce noise: comment out warning unless debugging EOF issues
             // fprintf(stderr, "Warning: Premature EOF reading inode %u (read %zu bytes)\n", inode_num, bytes_read);
             memset(inode_buf, 0, sizeof(exfs2_inode)); // Treat as invalid/empty
             fclose(fp);
             return -1; // Indicate failure on partial read
         } else {
            perror("Error reading inode from inode segment");
            fclose(fp);
            return -1;
         }
    }
    fclose(fp);
    return 0;
}

// Writes an inode to the appropriate segment file
int write_inode(uint32_t inode_num, const exfs2_inode *inode_buf) {
    int segment_index = inode_num / USABLE_INODES_PER_SEGMENT;
    int item_index_in_segment = inode_num % USABLE_INODES_PER_SEGMENT;
    // Inodes start after the bitmap block(s)
    int block_offset_in_segment = item_index_in_segment + BITMAP_BLOCKS_PER_SEGMENT;
    off_t offset = (off_t)block_offset_in_segment * BLOCK_SIZE;

    // Use "r+b" to modify existing file
    FILE *fp = get_segment_fp(INODE_SEGMENT_PREFIX, segment_index, "r+b", true); // Read/Write binary
    if (!fp) return -1;

    if (fseek(fp, offset, SEEK_SET) != 0) {
        perror("Error seeking in inode segment for write");
        fclose(fp);
        return -1;
    }
    if (fwrite(inode_buf, 1, sizeof(exfs2_inode), fp) != sizeof(exfs2_inode)) {
        perror("Error writing inode to inode segment");
        fclose(fp);
        return -1;
    }
    // Optimization: Consider removing fclose/fopen for consecutive writes to same segment
    fclose(fp);
    return 0;
}

// --- Allocation and Deallocation ---

// Generic allocation: finds free bit, sets it, returns global number
// Returns UINT32_MAX on failure.
uint32_t allocate_generic(const char *prefix, int *max_segment_idx_ptr, int usable_items_per_seg) {
    char bitmap_block[BLOCK_SIZE];
    int current_max_idx = *max_segment_idx_ptr;
    bool is_data_segment = (strcmp(prefix, DATA_SEGMENT_PREFIX) == 0);
    bool is_inode_segment = (strcmp(prefix, INODE_SEGMENT_PREFIX) == 0);

    // Scan existing segments first
    for (int seg_idx = 0; seg_idx <= current_max_idx; ++seg_idx) {
        FILE *fp = get_segment_fp(prefix, seg_idx, "r+b", false);
        if (!fp) continue;

        // Seek to bitmap (block 0)
        if (fseek(fp, 0, SEEK_SET) != 0 || fread(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE) {
             if (feof(fp)) {
                 fprintf(stderr, "Warning: EOF reading bitmap for %s%d during allocation\n", prefix, seg_idx);
                 memset(bitmap_block, 0xFF, BLOCK_SIZE); // Assume full if unreadable
             } else {
                perror("Error reading bitmap block during allocation");
                fclose(fp);
                continue;
             }
        }

        // Find free bit (special handling for root inode 0 and data block 0)
        int free_bit_index = -1;
        for (int i = 0; i < usable_items_per_seg; ++i) {
            // Skip inode 0 for regular allocation
            if (is_inode_segment && seg_idx == 0 && i == ROOT_INODE_NUM) {
                continue;
            }
            // Skip data block 0 for regular allocation
            if (is_data_segment && seg_idx == 0 && i == 0) {
                 continue;
            }

            if (!is_bit_set(bitmap_block, i)) {
                free_bit_index = i;
                break;
            }
        }


        if (free_bit_index != -1) {
            set_bit(bitmap_block, free_bit_index);
            // Seek back to bitmap (block 0) and write
            if (fseek(fp, 0, SEEK_SET) != 0 || fwrite(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE) {
                perror("Error writing updated bitmap block");
                // Attempt to clear bit back? Risky.
                clear_bit(bitmap_block, free_bit_index); // Revert in-memory for safety?
                fclose(fp);
                return UINT32_MAX; // Indicate allocation failure
            }
            fclose(fp);
            uint32_t global_num = (uint32_t)seg_idx * usable_items_per_seg + free_bit_index;
            return global_num;
        }
        fclose(fp);
    }

    // No free slots in existing segments, create a new one
    int new_segment_idx = current_max_idx + 1;
    FILE *fp = get_segment_fp(prefix, new_segment_idx, "r+b", true);
    if (!fp) {
        fprintf(stderr, "Failed to create new segment %s%d\n", prefix, new_segment_idx);
        return UINT32_MAX;
    }

    memset(bitmap_block, 0, BLOCK_SIZE); // New bitmap is all free
    // Find first free bit (will be 0 unless it's data segment 0, which is impossible here)
    int first_free_bit = -1;
     for (int i = 0; i < usable_items_per_seg; ++i) {
            // Skip inode 0 for regular allocation (not possible in new seg > 0)
            // Skip data block 0 for regular allocation (not possible in new seg > 0)
            if (!is_bit_set(bitmap_block, i)) { // Should find bit 0 immediately
                first_free_bit = i;
                break;
            }
     }

    if (first_free_bit == -1) { // Should never happen with a fresh segment
         fprintf(stderr, "Error: No usable bits in new segment %s%d?\n", prefix, new_segment_idx);
         fclose(fp);
         return UINT32_MAX;
    }

    set_bit(bitmap_block, first_free_bit);

     // Write bitmap (block 0)
     if (fseek(fp, 0, SEEK_SET) != 0 || fwrite(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE) {
        perror("Error writing initial bitmap to new segment");
        fclose(fp);
        // No need to revert bit, segment creation failed effectively
        return UINT32_MAX;
     }
    fclose(fp);

    *max_segment_idx_ptr = new_segment_idx;
    uint32_t global_num = (uint32_t)new_segment_idx * usable_items_per_seg + first_free_bit;
    return global_num;
}

// Allocates an inode. Returns inode number > 0.
// Returns UINT32_MAX on failure.
uint32_t allocate_inode() {
    // Inode 0 is handled specially during initialization.
    // This function allocates inodes > 0.
    uint32_t inode_num = allocate_generic(INODE_SEGMENT_PREFIX, &max_inode_segment_idx, USABLE_INODES_PER_SEGMENT);

    if (inode_num == UINT32_MAX) {
         fprintf(stderr, "allocate_inode: Failed to allocate inode.\n");
         return UINT32_MAX; // Propagate failure
    }

    // Zero-initialize the newly allocated inode on disk
    exfs2_inode new_inode = {0};
    if (write_inode(inode_num, &new_inode) != 0) {
        fprintf(stderr, "Failed to zero-initialize newly allocated inode %u\n", inode_num);
        free_inode(inode_num); // Free the bitmap entry if init fails
        return UINT32_MAX; // Indicate failure
    }

    return inode_num;
}

// Allocates a data block. Returns block number > 0, or 0 on failure.
uint32_t allocate_block() {
    // Block 0 is handled specially. This function allocates blocks > 0.
    uint32_t block_num = allocate_generic(DATA_SEGMENT_PREFIX, &max_data_segment_idx, USABLE_DATA_BLOCKS_PER_SEGMENT);

    if (block_num == UINT32_MAX) {
        fprintf(stderr, "allocate_block: Failed to allocate block.\n");
        return 0; // Use 0 to indicate data block allocation failure
    }

    // Optional: Zero out block? Generally good practice.
    char zero_buffer[BLOCK_SIZE] = {0};
    if (write_block(block_num, zero_buffer) != 0) {
        fprintf(stderr, "Failed to zero-initialize newly allocated block %u\n", block_num);
        free_block(block_num); // Free the bitmap entry if init fails
        return 0; // Indicate failure
    }

    return block_num;
}


// Frees an item (inode or block) in the bitmap
void free_generic(uint32_t global_num, const char *prefix, int usable_items_per_seg) {
     bool is_inode = (strcmp(prefix, INODE_SEGMENT_PREFIX) == 0);
     if (is_inode && global_num == ROOT_INODE_NUM) {
         fprintf(stderr, "Warning: Attempt to free root inode %u - operation ignored.\n", ROOT_INODE_NUM);
         return;
     }
     // Data block 0 is implicitly used by root inode, prevent explicit free?
     if (!is_inode && global_num == 0) {
         fprintf(stderr, "Warning: Attempt to free data block 0 - operation ignored.\n");
         return;
     }

    int segment_index = global_num / usable_items_per_seg;
    int bit_index_in_segment = global_num % usable_items_per_seg;

    // Bounds check for segment index
    int current_max_idx = is_inode ? max_inode_segment_idx : max_data_segment_idx;
    if (segment_index > current_max_idx || segment_index < 0) {
         fprintf(stderr, "Error: Attempt to free item %u from non-existent segment %s%d\n", global_num, prefix, segment_index);
         return;
    }


    FILE *fp = get_segment_fp(prefix, segment_index, "r+b", false);
    if (!fp) {
        fprintf(stderr, "Error opening segment %s%d to free item %u\n", prefix, segment_index, global_num);
        return;
    }

    char bitmap_block[BLOCK_SIZE];
    // Read bitmap (block 0)
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE) {
        perror("Error reading bitmap block for freeing");
        fclose(fp);
        return;
    }

    if (!is_bit_set(bitmap_block, bit_index_in_segment)) {
        fprintf(stderr, "Warning: Attempting to free already free item %u in %s%d\n", global_num, prefix, segment_index);
    }

    clear_bit(bitmap_block, bit_index_in_segment);

    // Write updated bitmap back (block 0)
    if (fseek(fp, 0, SEEK_SET) != 0 || fwrite(bitmap_block, 1, BLOCK_SIZE, fp) != BLOCK_SIZE) {
        perror("Error writing updated bitmap block after freeing");
    }
    fclose(fp);
}

void free_inode(uint32_t inode_num) {
    free_generic(inode_num, INODE_SEGMENT_PREFIX, USABLE_INODES_PER_SEGMENT);
}

void free_block(uint32_t block_num) {
    free_generic(block_num, DATA_SEGMENT_PREFIX, USABLE_DATA_BLOCKS_PER_SEGMENT);
}

// --- Initialization ---
// This function is now called implicitly at the start of main.
// It ensures the basic filesystem structure (segment 0 for inodes and data) exists.
void initialize_exfs2() {
    // Check if root inode segment exists (basic check)
    FILE *fp_inode0 = get_segment_fp(INODE_SEGMENT_PREFIX, 0, "rb", false);
    bool needs_init = (fp_inode0 == NULL);
    if (fp_inode0) fclose(fp_inode0);

    // Always scan for max indices
    int i = 0;
    char fname[256];
    max_inode_segment_idx = -1; // Reset before scan
    while (true) {
        snprintf(fname, sizeof(fname), INODE_SEGMENT_PREFIX "%d" SEGMENT_SUFFIX, i);
        if (access(fname, F_OK) == 0) max_inode_segment_idx = i; else break;
        i++;
    }
    i = 0;
    max_data_segment_idx = -1; // Reset before scan
     while (true) {
        snprintf(fname, sizeof(fname), DATA_SEGMENT_PREFIX "%d" SEGMENT_SUFFIX, i);
        if (access(fname, F_OK) == 0) max_data_segment_idx = i; else break;
        i++;
    }

    // If segment 0 doesn't exist or wasn't found, perform initial setup.
    if (needs_init || max_inode_segment_idx < 0 || max_data_segment_idx < 0) {
        printf("Initializing ExFS2 filesystem structure (Segment 0)...\n");

        // Ensure segment 0 files exist and are large enough
        FILE *fp_i = get_segment_fp(INODE_SEGMENT_PREFIX, 0, "r+b", true);
        if (!fp_i) { fprintf(stderr, "Failed init inode seg 0\n"); exit(EXIT_FAILURE); }
        fclose(fp_i);
        FILE *fp_d = get_segment_fp(DATA_SEGMENT_PREFIX, 0, "r+b", true);
        if (!fp_d) { fprintf(stderr, "Failed init data seg 0\n"); exit(EXIT_FAILURE); }
        fclose(fp_d);

        max_inode_segment_idx = 0; // Set explicitly after creation
        max_data_segment_idx = 0;

        // Mark inode 0 used in its bitmap (inode segment 0, block 0)
        char inode_bitmap_block[BLOCK_SIZE] = {0}; // Start fresh
        set_bit(inode_bitmap_block, ROOT_INODE_NUM);
        FILE* fp_i0_bitmap = get_segment_fp(INODE_SEGMENT_PREFIX, 0, "r+b", false); // Should exist now
        if (!fp_i0_bitmap) { perror("Failed open inode 0 for bitmap init"); exit(EXIT_FAILURE); }
        if (fseek(fp_i0_bitmap, 0, SEEK_SET)!=0 || fwrite(inode_bitmap_block, 1, BLOCK_SIZE, fp_i0_bitmap) != BLOCK_SIZE) { perror("Init write inode bitmap"); fclose(fp_i0_bitmap); exit(EXIT_FAILURE); }
        fclose(fp_i0_bitmap);

        // Mark data block 0 used in its bitmap (data segment 0, block 0)
        uint32_t root_data_block = 0; // Data block 0 used for root dir
        char data_bitmap_block[BLOCK_SIZE] = {0}; // Start fresh
        set_bit(data_bitmap_block, root_data_block);
        FILE* fp_d0_bitmap = get_segment_fp(DATA_SEGMENT_PREFIX, 0, "r+b", false); // Should exist now
        if (!fp_d0_bitmap) { perror("Failed open data 0 for bitmap init"); exit(EXIT_FAILURE); }
        if (fseek(fp_d0_bitmap, 0, SEEK_SET)!=0 || fwrite(data_bitmap_block, 1, BLOCK_SIZE, fp_d0_bitmap) != BLOCK_SIZE) { perror("Init write data bitmap"); fclose(fp_d0_bitmap); exit(EXIT_FAILURE); }
        fclose(fp_d0_bitmap);


        // Initialize root inode structure (inode 0 itself)
        exfs2_inode root_inode_data = {0};
        root_inode_data.mode = EXFS2_IFDIR;
        root_inode_data.size = 0; // Will be updated below
        root_inode_data.direct_blocks[0] = root_data_block; // Points to its data (block 0)
        // All other pointers are 0

        // Initialize root data block (data block 0) with "." and ".."
        char root_dir_block_buf[BLOCK_SIZE] = {0};
        exfs2_dirent *dir_entries = (exfs2_dirent *)root_dir_block_buf;

        // Entry for "."
        dir_entries[0].inode_num = ROOT_INODE_NUM; // "." -> self (inode 0)
        strncpy(dir_entries[0].name, ".", MAX_FILENAME_LEN);
        dir_entries[0].name[MAX_FILENAME_LEN] = '\0'; // Ensure null termination
        root_inode_data.size += sizeof(exfs2_dirent);

        // Entry for ".."
        dir_entries[1].inode_num = ROOT_INODE_NUM; // ".." -> self (inode 0 for root)
        strncpy(dir_entries[1].name, "..", MAX_FILENAME_LEN);
        dir_entries[1].name[MAX_FILENAME_LEN] = '\0'; // Ensure null termination
        root_inode_data.size += sizeof(exfs2_dirent);

        // Write root data block (block 0) and root inode (inode 0)
        if (write_block(root_data_block, root_dir_block_buf) != 0) {
            fprintf(stderr, "Initialization error: Failed write root data block\n"); exit(EXIT_FAILURE);
        }
        if (write_inode(ROOT_INODE_NUM, &root_inode_data) != 0) {
            fprintf(stderr, "Initialization error: Failed write root inode\n"); exit(EXIT_FAILURE);
        }
        printf("ExFS2 structure initialized.\n");
    } else {
         // Optionally print status if already initialized
         // printf("ExFS2 file system structure verified.\n");
         // printf("Max Inode Segment Index: %d\n", max_inode_segment_idx);
         // printf("Max Data Segment Index: %d\n", max_data_segment_idx);
    }
}


// --- Path Traversal ---
uint32_t find_entry_in_dir(uint32_t dir_inode_num, const char *name) {
    exfs2_inode dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0) return 0; // Use 0 to indicate not found / error
    if (!(dir_inode.mode & EXFS2_IFDIR)) return 0; // Not a directory

    char block_buf[BLOCK_SIZE];

    // Search direct blocks
    for (int i = 0; i < NUM_DIRECT; ++i) {
        uint32_t current_data_block = dir_inode.direct_blocks[i];
        if (current_data_block == 0) continue; // Skip unused block pointers

        if (read_block(current_data_block, block_buf) != 0) {
            fprintf(stderr, "Warning: Failed to read data block %u for dir inode %u\n", current_data_block, dir_inode_num);
            continue; // Try next block
        }

        exfs2_dirent *entries = (exfs2_dirent *)block_buf;
        for (int j = 0; j < DIRENTS_PER_BLOCK; ++j) {
            // Check if entry is valid (inode_num != 0) and name matches
            if (entries[j].inode_num != 0 && strncmp(entries[j].name, name, MAX_FILENAME_LEN) == 0) {
                return entries[j].inode_num; // Found it!
            }
        }
    }

    // TODO: Search indirect blocks for find_entry_in_dir
    if (dir_inode.single_indirect != 0) {
        fprintf(stderr, "Warning: find_entry_in_dir does not search single indirect blocks yet.\n");
    }
    if (dir_inode.double_indirect != 0) {
        fprintf(stderr, "Warning: find_entry_in_dir does not search double indirect blocks yet.\n");
    }
    if (dir_inode.triple_indirect != 0) {
        fprintf(stderr, "Warning: find_entry_in_dir does not search triple indirect blocks yet.\n");
    }

    return 0; // Not found
}

// Traverses a path, returning the inode number of the final component.
// If create_missing is true, it will return the inode number of the *parent*
// directory if the final component doesn't exist, and *last_component* will
// contain the name of the missing component. Intermediate directories can
// also be created if create_missing is true.
// Returns 0 on success, -1 on error.
int traverse_path(const char *path, uint32_t *result_inode_num, bool create_missing, char *last_component) {
    if (path == NULL) {
        fprintf(stderr, "traverse_path: NULL path provided.\n");
        return -1;
    }
    char *path_copy = strdup(path);
    if (!path_copy) {
        perror("strdup in traverse_path");
        return -1;
    }

    // Handle absolute paths - skip leading slashes
    char *current_path = path_copy;
    while (current_path[0] == '/') {
        current_path++;
    }

    uint32_t current_inode_num = ROOT_INODE_NUM;
    *result_inode_num = ROOT_INODE_NUM; // Default if path is effectively "/"

    // If path was only slashes (or empty after stripping slashes), it refers to root.
    if (strlen(current_path) == 0) {
        if (last_component) strcpy(last_component, "/"); // Indicate root as last component conceptually
        free(path_copy);
        return 0;
    }

    char *token;
    char *rest = current_path;
    char component[MAX_FILENAME_LEN + 1];
    uint32_t parent_inode_num = ROOT_INODE_NUM;

    while ((token = strtok_r(rest, "/", &rest))) {
        strncpy(component, token, MAX_FILENAME_LEN);
        component[MAX_FILENAME_LEN] = '\0';

        // Before looking for the next component, ensure the current inode is a directory
        exfs2_inode current_inode_data;
        // ***** FIX: Corrected variable name and added & *****
        if (read_inode(current_inode_num, &current_inode_data) != 0) {
             fprintf(stderr, "traverse_path: Failed read inode %u for component '%s'\n", current_inode_num, component);
             free(path_copy);
             return -1;
        }
        if (!(current_inode_data.mode & EXFS2_IFDIR)) {
            fprintf(stderr, "traverse_path: Path component error - inode %u is not a directory when trying to process '%s'\n", current_inode_num, component); // Improve error msg
            free(path_copy);
            return -1;
        }

        parent_inode_num = current_inode_num;
        uint32_t next_inode_num = find_entry_in_dir(current_inode_num, component);
        bool is_last_component = (rest == NULL || strlen(rest) == 0);

        if (next_inode_num == 0) { // Component Not Found
            if (create_missing && is_last_component) {
                // We need to create this final component. Return the parent inode
                // and the name of the component to be created.
                *result_inode_num = parent_inode_num;
                if (last_component) {
                    strncpy(last_component, component, MAX_FILENAME_LEN + 1);
                    last_component[MAX_FILENAME_LEN] = '\0'; // Ensure null termination
                }
                free(path_copy);
                return 0; // Signal success, indicating parent and missing component name
            } else if (create_missing) { // Create intermediate directory
                fprintf(stdout, "Auto-creating intermediate directory: %s\n", component);

                // 1. Allocate inode and data block for the new directory
                uint32_t new_dir_inode_num = allocate_inode();
                if (new_dir_inode_num == UINT32_MAX) {
                    fprintf(stderr, "traverse_path: Failed to allocate inode for intermediate dir '%s'\n", component);
                    free(path_copy); return -1;
                }
                uint32_t new_dir_block_num = allocate_block();
                if (new_dir_block_num == 0) {
                    fprintf(stderr, "traverse_path: Failed to allocate data block for intermediate dir '%s'\n", component);
                    free_inode(new_dir_inode_num); // Clean up allocated inode
                    free(path_copy); return -1;
                }

                // 2. Initialize the new directory's inode
                exfs2_inode new_dir_inode = {0};
                new_dir_inode.mode = EXFS2_IFDIR;
                new_dir_inode.direct_blocks[0] = new_dir_block_num;
                new_dir_inode.size = 0; // Will update below

                // 3. Initialize the new directory's data block with "." and ".."
                char new_dir_block_buf[BLOCK_SIZE] = {0};
                exfs2_dirent *entries = (exfs2_dirent *)new_dir_block_buf;
                // "." entry
                entries[0].inode_num = new_dir_inode_num;
                strncpy(entries[0].name, ".", MAX_FILENAME_LEN); entries[0].name[MAX_FILENAME_LEN] = '\0';
                new_dir_inode.size += sizeof(exfs2_dirent);
                // ".." entry
                entries[1].inode_num = parent_inode_num; // Points to the parent directory
                strncpy(entries[1].name, "..", MAX_FILENAME_LEN); entries[1].name[MAX_FILENAME_LEN] = '\0';
                new_dir_inode.size += sizeof(exfs2_dirent);

                // 4. Write the new directory's data block and inode to disk
                if (write_block(new_dir_block_num, new_dir_block_buf) != 0) {
                    fprintf(stderr, "traverse_path: Failed write data block %u for new dir '%s'\n", new_dir_block_num, component);
                    free_inode(new_dir_inode_num); free_block(new_dir_block_num); free(path_copy); return -1;
                }
                if (write_inode(new_dir_inode_num, &new_dir_inode) != 0) {
                    fprintf(stderr, "traverse_path: Failed write inode %u for new dir '%s'\n", new_dir_inode_num, component);
                    // Data block already written, try to free both
                    free_inode(new_dir_inode_num); free_block(new_dir_block_num); free(path_copy); return -1;
                }

                // 5. Add the entry for the new directory into its parent directory
                if (add_entry_to_dir(parent_inode_num, component, new_dir_inode_num) != 0) {
                    fprintf(stderr, "traverse_path: Failed add entry '%s' to parent dir inode %u\n", component, parent_inode_num);
                    recursive_free(new_dir_inode_num); // Try to clean up the created dir fully
                    free(path_copy); return -1;
                }

                next_inode_num = new_dir_inode_num; // Continue traversal into the newly created directory
            } else { // Not found, and not creating
                fprintf(stderr, "Path not found: Component '%s' does not exist in directory inode %u\n", component, parent_inode_num);
                free(path_copy);
                return -1; // Signal failure: path not found
            }
        } // End of component not found block

        // Move to the next component
        current_inode_num = next_inode_num;

        // If this was the last component processed, record its name if requested
        if (is_last_component && last_component) {
             strncpy(last_component, component, MAX_FILENAME_LEN + 1);
             last_component[MAX_FILENAME_LEN] = '\0'; // Ensure null termination
        }
    } // end while loop over tokens

    *result_inode_num = current_inode_num; // Final component's inode
    free(path_copy);
    return 0; // Success
}


// --- Directory Operations ---

// Adds a directory entry to a parent directory.
// Returns 0 on success, -1 on failure.
int add_entry_to_dir(uint32_t parent_inode_num, const char *name, uint32_t entry_inode_num) {
     exfs2_inode parent_inode;
     if (read_inode(parent_inode_num, &parent_inode) != 0) {
         fprintf(stderr, "add_entry_to_dir: Failed read parent inode %u\n", parent_inode_num);
         return -1;
     }
     if (!(parent_inode.mode & EXFS2_IFDIR)) {
         fprintf(stderr, "add_entry_to_dir: Parent inode %u is not a directory\n", parent_inode_num);
         return -1;
     }

     char block_buf[BLOCK_SIZE];
     int allocated_new_block_idx = -1; // Track if we allocated a new block

      // Try finding an empty slot in existing direct blocks
      for (int i = 0; i < NUM_DIRECT; ++i) {
          uint32_t block_num = parent_inode.direct_blocks[i];
          if (block_num == 0) continue; // Skip unallocated direct pointers

          if (read_block(block_num, block_buf) != 0) {
              fprintf(stderr, "Warning: add_entry_to_dir: Failed read data block %u for dir inode %u\n", block_num, parent_inode_num);
              continue;
          }

          exfs2_dirent *entries = (exfs2_dirent *)block_buf;
          for (int j = 0; j < DIRENTS_PER_BLOCK; ++j) {
              if (entries[j].inode_num == 0) { // Found free slot
                  entries[j].inode_num = entry_inode_num;
                  strncpy(entries[j].name, name, MAX_FILENAME_LEN);
                  entries[j].name[MAX_FILENAME_LEN] = '\0'; // Ensure null termination

                  if (write_block(block_num, block_buf) != 0) {
                      fprintf(stderr, "add_entry_to_dir: Failed write block %u updating entry\n", block_num);
                      return -1;
                  }
                  // Update parent inode size (simplistic)
                  parent_inode.size += sizeof(exfs2_dirent);
                  if (write_inode(parent_inode_num, &parent_inode) != 0) {
                       fprintf(stderr, "add_entry_to_dir: Failed write parent inode %u after adding entry\n", parent_inode_num);
                      return -1;
                  }
                  return 0; // Success!
              }
          }
      } // End loop searching existing blocks

      // No empty slot found, try allocating a new direct block if available
      for (int i = 0; i < NUM_DIRECT; ++i) {
           if (parent_inode.direct_blocks[i] == 0) {
               uint32_t new_block_num = allocate_block();
               if (new_block_num == 0) {
                   fprintf(stderr, "add_entry_to_dir: Failed to allocate new block for dir inode %u\n", parent_inode_num);
                   return -1; // Allocation failed
               }
               parent_inode.direct_blocks[i] = new_block_num;
               allocated_new_block_idx = i; // Remember which block we added

               // Initialize the new block (it should be zeroed by allocate_block)
               memset(block_buf, 0, BLOCK_SIZE);
               exfs2_dirent *entries = (exfs2_dirent *)block_buf;
               entries[0].inode_num = entry_inode_num;
               strncpy(entries[0].name, name, MAX_FILENAME_LEN);
               entries[0].name[MAX_FILENAME_LEN] = '\0';

               // Write the new block content
               if (write_block(new_block_num, block_buf) != 0) {
                   fprintf(stderr, "add_entry_to_dir: Failed write new block %u\n", new_block_num);
                   free_block(new_block_num); // Clean up allocated block
                   parent_inode.direct_blocks[i] = 0; // Reset pointer in memory
                   return -1;
               }

               // Update parent inode size and write it back
               parent_inode.size += sizeof(exfs2_dirent);
               if (write_inode(parent_inode_num, &parent_inode) != 0) {
                    fprintf(stderr, "add_entry_to_dir: Failed write parent inode %u after adding new block\n", parent_inode_num);
                    // Try to clean up: zero entry, free block
                    memset(block_buf, 0, BLOCK_SIZE);
                    write_block(new_block_num, block_buf); // Try to revert block write
                    free_block(new_block_num);
                    return -1;
               }
               return 0; // Success!
           }
      }

     // TODO: Add entry using indirect blocks if direct blocks are full
     fprintf(stderr, "add_entry_to_dir: Directory inode %u full (direct blocks only).\n", parent_inode_num);
     if (allocated_new_block_idx != -1) {
         // This case should not be reachable if the allocation logic worked
         fprintf(stderr, "Internal error: allocated block but didn't use it?\n");
         free_block(parent_inode.direct_blocks[allocated_new_block_idx]);
         parent_inode.direct_blocks[allocated_new_block_idx] = 0;
     }
     return -1; // Directory full (or indirect not implemented)
}


// Removes a directory entry from a parent directory.
// Note: Does NOT free the inode/blocks of the removed entry.
// Returns 0 on success, -1 on failure.
int remove_entry_from_dir(uint32_t parent_inode_num, const char *name) {
     exfs2_inode parent_inode;
     if (read_inode(parent_inode_num, &parent_inode) != 0) {
          fprintf(stderr, "remove_entry_from_dir: Failed read parent inode %u\n", parent_inode_num);
          return -1;
     }
     if (!(parent_inode.mode & EXFS2_IFDIR)) {
          fprintf(stderr, "remove_entry_from_dir: Parent inode %u is not a directory\n", parent_inode_num);
          return -1;
     }

     char block_buf[BLOCK_SIZE];
     bool found = false;

      // Search direct blocks
      for (int i = 0; i < NUM_DIRECT; ++i) {
          uint32_t block_num = parent_inode.direct_blocks[i];
          if (block_num == 0) continue; // Skip unused blocks

          if (read_block(block_num, block_buf) != 0) {
              fprintf(stderr, "Warning: remove_entry_from_dir: Failed read data block %u for dir inode %u\n", block_num, parent_inode_num);
              continue;
          }

          exfs2_dirent *entries = (exfs2_dirent *)block_buf;
          for (int j = 0; j < DIRENTS_PER_BLOCK; ++j) {
              if (entries[j].inode_num != 0 && strcmp(entries[j].name, name) == 0) {
                  // FIX: Removed unused variable inode_num_being_removed
                  entries[j].inode_num = 0; // Mark as unused by setting inode to 0
                  memset(entries[j].name, 0, MAX_FILENAME_LEN + 1); // Clear name for tidiness
                  found = true;

                  if (write_block(block_num, block_buf) != 0) {
                      fprintf(stderr, "remove_entry_from_dir: Failed write block %u after removing entry\n", block_num);
                      return -1;
                  }
                  goto end_search; // Exit loops once found and removed
              }
          }
      }

      // TODO: Indirect blocks for remove_entry_from_dir
      if (parent_inode.single_indirect != 0) {
          fprintf(stderr, "Warning: remove_entry_from_dir does not search single indirect blocks yet.\n");
      }
      // ... double/triple ...

end_search:
     if (!found) {
         fprintf(stderr, "remove_entry_from_dir: Entry '%s' not found in dir inode %u.\n", name, parent_inode_num);
         return -1;
     }
     return 0; // Success
}

// --- Command Implementations ---

// Lists directory contents recursively.
void list_directory(uint32_t dir_inode_num, int indent) {
    exfs2_inode dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0) {
        fprintf(stderr, "list_directory: Failed to read inode %u\n", dir_inode_num);
        return;
    }
    if (!(dir_inode.mode & EXFS2_IFDIR)) {
        // This case should ideally be caught before calling list_directory (e.g., in main)
        fprintf(stderr, "list_directory: Error - inode %u is not a directory.\n", dir_inode_num);
        return;
    }

    char block_buf[BLOCK_SIZE];

    // Iterate through direct blocks
    for (int i = 0; i < NUM_DIRECT; ++i) {
        uint32_t block_num = dir_inode.direct_blocks[i];
        if (block_num == 0) continue; // Skip unused blocks

        if (read_block(block_num, block_buf) != 0) {
            fprintf(stderr, "Warning: list_directory: Failed to read data block %u for dir inode %u\n", block_num, dir_inode_num);
            continue;
        }

        exfs2_dirent *entries = (exfs2_dirent *)block_buf;
        for (int j = 0; j < DIRENTS_PER_BLOCK; ++j) {
            if (entries[j].inode_num != 0) { // Check if the entry is valid
                // Print indentation
                for (int k = 0; k < indent; ++k) printf("  ");

                // Check entry type for listing purposes
                exfs2_inode entry_inode;
                bool is_dir = false;
                const char *suffix = "";
                uint64_t entry_size = 0; // Show size
                if (read_inode(entries[j].inode_num, &entry_inode) == 0) {
                     entry_size = entry_inode.size;
                     if (entry_inode.mode & EXFS2_IFDIR) {
                         is_dir = true;
                         suffix = "/";
                     }
                 } else {
                     fprintf(stderr, "\nWarning: list_directory: Couldn't read inode %u listed in dir %u\n", entries[j].inode_num, dir_inode_num);
                     suffix = " (inode read error)";
                 }

                 // Print name, suffix, inode number, and size
                 printf("%s%s (inode %u, size %" PRIu64 ")\n", entries[j].name, suffix, entries[j].inode_num, entry_size);

                 // Recurse into subdirectories (excluding "." and "..")
                 if (is_dir && strcmp(entries[j].name,".")!=0 && strcmp(entries[j].name,"..")!=0) {
                     list_directory(entries[j].inode_num, indent + 1);
                 }
            }
        }
    }

    // TODO: Indirect blocks for list_directory
     if (dir_inode.single_indirect != 0) {
        fprintf(stderr, "Warning: list_directory does not list contents from single indirect blocks yet.\n");
    }
    // ... double/triple ...
}

// Gets the data block number corresponding to a logical block index within a file.
// Handles direct and single indirect blocks. Allocates blocks if needed and requested.
// Returns block number > 0 on success, 0 on error or if block not allocated and allocate_if_needed is false.
uint32_t get_block_num_for_file_offset(exfs2_inode *inode, uint64_t offset, bool allocate_if_needed, uint32_t file_inode_num /* for writing back inode */) {
    if (!inode) {
        fprintf(stderr, "get_block_num_for_file_offset: NULL inode provided.\n");
        return 0;
    }

    uint64_t block_index_in_file = offset / BLOCK_SIZE;
    uint32_t block_num = 0;

    // Direct Blocks
    if (block_index_in_file < NUM_DIRECT) {
        block_num = inode->direct_blocks[block_index_in_file];
        if (block_num == 0 && allocate_if_needed) {
            block_num = allocate_block();
            if (block_num == 0) {
                fprintf(stderr, "get_block_num_for_file_offset: Failed to allocate direct block %" PRIu64 " for inode %u\n", block_index_in_file, file_inode_num);
                return 0; // Allocation failed
            }
            inode->direct_blocks[block_index_in_file] = block_num;
            // Write the parent inode back immediately to save the new block pointer
            if (write_inode(file_inode_num, inode) != 0) {
                fprintf(stderr, "get_block_num_for_file_offset: Failed write inode %u after allocating direct block\n", file_inode_num);
                // Try to free the allocated block if inode write failed
                free_block(block_num);
                inode->direct_blocks[block_index_in_file] = 0; // Revert in memory
                return 0;
            }
        }
        return block_num; // Return existing or newly allocated block number, or 0 if not allocated
    }

    // Single Indirect Block
    uint64_t direct_limit = NUM_DIRECT;
    uint64_t single_limit = direct_limit + POINTERS_PER_INDIRECT_BLOCK;
    if (block_index_in_file < single_limit) {
        // Ensure the single indirect block itself is allocated
        if (inode->single_indirect == 0) {
            if (!allocate_if_needed) return 0; // Cannot proceed without allocation

            inode->single_indirect = allocate_block();
            if (inode->single_indirect == 0) {
                fprintf(stderr, "get_block_num_for_file_offset: Failed to allocate single indirect block for inode %u\n", file_inode_num);
                return 0;
            }
            // Write inode back immediately to save the indirect block pointer
            if (write_inode(file_inode_num, inode) != 0) {
                fprintf(stderr, "get_block_num_for_file_offset: Failed write inode %u after allocating single indirect block\n", file_inode_num);
                free_block(inode->single_indirect); // Clean up
                inode->single_indirect = 0; // Revert in memory
                return 0;
            }
            // Zero out the newly allocated indirect block
            char zero_buf[BLOCK_SIZE] = {0};
            if (write_block(inode->single_indirect, zero_buf) != 0) {
                 fprintf(stderr, "get_block_num_for_file_offset: Failed to zero single indirect block %u\n", inode->single_indirect);
                 // Inode write succeeded, but indirect block init failed. Tricky state.
            }
        }

        // Read the single indirect block
        indirect_block indir_block;
        if (read_block(inode->single_indirect, (char *)&indir_block) != 0) {
            fprintf(stderr, "get_block_num_for_file_offset: Failed read single indirect block %u for inode %u\n", inode->single_indirect, file_inode_num);
            return 0;
        }

        // Calculate index within the indirect block
        uint32_t index_in_indirect = block_index_in_file - direct_limit;
        if (index_in_indirect >= POINTERS_PER_INDIRECT_BLOCK) {
             fprintf(stderr, "get_block_num_for_file_offset: Internal error - index %u out of bounds for indirect block\n", index_in_indirect);
             return 0;
        }
        block_num = indir_block.block_ptrs[index_in_indirect];

        if (block_num == 0 && allocate_if_needed) {
            block_num = allocate_block();
            if (block_num == 0) {
                fprintf(stderr, "get_block_num_for_file_offset: Failed allocate data block pointed by single indirect (index %u) for inode %u\n", index_in_indirect, file_inode_num);
                return 0;
            }
            indir_block.block_ptrs[index_in_indirect] = block_num;
            // Write the indirect block back to save the new data block pointer
            if (write_block(inode->single_indirect, (char *)&indir_block) != 0) {
                fprintf(stderr, "get_block_num_for_file_offset: Failed write single indirect block %u after allocating data block\n", inode->single_indirect);
                free_block(block_num); // Clean up allocated data block
                indir_block.block_ptrs[index_in_indirect] = 0; // Revert in memory
                return 0;
            }
        }
        return block_num; // Return existing or newly allocated block, or 0
    }

    // TODO: Double Indirect Blocks
    uint64_t double_limit = single_limit + (uint64_t)POINTERS_PER_INDIRECT_BLOCK * POINTERS_PER_INDIRECT_BLOCK;
     if (block_index_in_file < double_limit) {
         fprintf(stderr, "Double indirect blocks not implemented in get_block_num_for_file_offset.\n");
         return 0;
     }

    // TODO: Triple Indirect Blocks
    fprintf(stderr, "Offset %" PRIu64 " too large (Triple indirect blocks not implemented).\n", offset);
    return 0;
}

// Adds a local file to the ExFS2 filesystem.
void add_file(const char *exfs2_path, const char *local_path) {
    char parent_path_buf[1024] = {0};
    char filename_buf[MAX_FILENAME_LEN + 1] = {0};
    uint32_t parent_inode_num = ROOT_INODE_NUM; // Default to root initially
    // FIX: Removed unused variable parent_is_root

    // --- Robust Path Parsing ---
    char *path_copy_dir = strdup(exfs2_path);
    if (!path_copy_dir) { perror("strdup"); return; }
    char *dir_component = dirname(path_copy_dir);
    if (dir_component) {
        strncpy(parent_path_buf, dir_component, sizeof(parent_path_buf) - 1);
    } else {
        strcpy(parent_path_buf, "."); // Should not happen with valid strdup?
    }
    free(path_copy_dir);

    char *path_copy_base = strdup(exfs2_path);
    if (!path_copy_base) { perror("strdup"); return; }
    char *base_component = basename(path_copy_base);
     if (base_component) {
        strncpy(filename_buf, base_component, MAX_FILENAME_LEN);
    } else {
         fprintf(stderr, "add_file: Could not extract filename from '%s'\n", exfs2_path);
         free(path_copy_base);
         return;
    }
    free(path_copy_base);

    if (strlen(filename_buf) == 0 || strcmp(filename_buf, "/") == 0 || strcmp(filename_buf, ".") == 0 || strcmp(filename_buf, "..") == 0) {
        fprintf(stderr, "add_file: Invalid target filename '%s'\n", filename_buf);
        return;
    }

    printf("add_file: Target: '%s', Parsed Parent: '%s', Parsed Filename: '%s'\n", exfs2_path, parent_path_buf, filename_buf);

    // Handle the case where dirname is "." (meaning the path was just a filename)
    // or "/" (meaning the parent is root).
    if (strcmp(parent_path_buf, "/") == 0) {
        parent_inode_num = ROOT_INODE_NUM;
        printf("add_file: Parent directory is ROOT (inode %u)\n", parent_inode_num);
    } else if (strcmp(parent_path_buf, ".") == 0) {
        parent_inode_num = ROOT_INODE_NUM;
        printf("add_file: Parent directory is '.' -> treating as ROOT (inode %u)\n", parent_inode_num);
    } else {
        // Traverse to the explicit parent directory, creating intermediate dirs
        printf("add_file: Traversing to parent directory '%s'\n", parent_path_buf);
        if (traverse_path(parent_path_buf, &parent_inode_num, true, NULL) != 0) {
            fprintf(stderr, "add_file: Failed to traverse or create parent path '%s'\n", parent_path_buf);
            return;
        }
        printf("add_file: Resolved parent directory '%s' to inode %u\n", parent_path_buf, parent_inode_num);
    }
    // --- End Path Parsing ---


     // Check if the file *already* exists in the resolved parent directory
     if (find_entry_in_dir(parent_inode_num, filename_buf) != 0) {
         fprintf(stderr, "add_file: Error - File '%s' already exists in target directory (inode %u)\n", filename_buf, parent_inode_num);
         return;
     }

     // Open the local file for reading
     FILE *local_fp = fopen(local_path, "rb");
     if (!local_fp) {
         perror("add_file: Failed to open local file for reading");
         fprintf(stderr, "  Local path: %s\n", local_path);
         return;
     }

     // Allocate an inode for the new file
     uint32_t new_file_inode_num = allocate_inode();
     if (new_file_inode_num == UINT32_MAX) {
         fprintf(stderr, "add_file: Failed to allocate inode for '%s'\n", filename_buf);
         fclose(local_fp);
         return;
     }
     printf("add_file: Allocated inode %u for new file '%s'\n", new_file_inode_num, filename_buf);

     // Initialize the new file's inode (in memory)
     exfs2_inode new_file_inode = {0};
     new_file_inode.mode = EXFS2_IFREG; // Mark as regular file
     new_file_inode.size = 0; // Will be updated as we write blocks

     char buffer[BLOCK_SIZE];
     size_t bytes_read;
     uint64_t current_offset = 0;
     bool write_error = false;

     // Read local file and write to ExFS2 blocks
     while ((bytes_read = fread(buffer, 1, BLOCK_SIZE, local_fp)) > 0) {
         // Get the target block number for the current file offset, allocating if needed
         uint32_t target_block_num = get_block_num_for_file_offset(&new_file_inode, current_offset, true, new_file_inode_num);
         if (target_block_num == 0) {
             fprintf(stderr, "add_file: Failed to get/allocate block for offset %" PRIu64 "\n", current_offset);
             write_error = true;
             break;
         }

         // Zero out the remainder of the buffer if it's a partial read (last block)
         if (bytes_read < BLOCK_SIZE) {
             memset(buffer + bytes_read, 0, BLOCK_SIZE - bytes_read);
         }

         // Write the data block
         if (write_block(target_block_num, buffer) != 0) {
             fprintf(stderr, "add_file: Failed to write data block %u\n", target_block_num);
             write_error = true;
             break;
         }

         // Update file size and offset
         // Note: Size update must happen *before* writing the inode potentially inside get_block_num_for_file_offset
         new_file_inode.size += bytes_read;
         current_offset += bytes_read; // Use actual bytes read for offset increment
     }

     if (ferror(local_fp)) {
         perror("add_file: Error reading from local file");
         write_error = true;
     }
     fclose(local_fp);

     // If any error occurred during write, clean up the allocated inode and blocks
     if (write_error) {
         fprintf(stderr, "add_file: Cleaning up due to write error for '%s'\n", exfs2_path);
         recursive_free(new_file_inode_num); // Free inode and its data blocks
         return;
     }

     // Write the final inode data (with correct size, block pointers) to disk ONE LAST TIME
     // This ensures the final size is correct, even if get_block_num didn't write it last.
     if (write_inode(new_file_inode_num, &new_file_inode) != 0) {
         fprintf(stderr, "add_file: Failed to write final inode %u for '%s'\n", new_file_inode_num, filename_buf);
         recursive_free(new_file_inode_num); // Clean up
         return;
     }

     // Add the directory entry for the new file into the parent directory
     if (add_entry_to_dir(parent_inode_num, filename_buf, new_file_inode_num) != 0) {
         fprintf(stderr, "add_file: Failed to add directory entry '%s' to parent inode %u\n", filename_buf, parent_inode_num);
         // Inode and data are written, but entry failed. Clean up the file itself.
         recursive_free(new_file_inode_num);
         return;
     }

     printf("Successfully added '%s' to ExFS2 at '%s' (inode %u, size %" PRIu64 " bytes)\n", local_path, exfs2_path, new_file_inode_num, new_file_inode.size);
}


// Extracts a file from ExFS2 to standard output.
void extract_file(const char *exfs2_path) {
     uint32_t file_inode_num;
     char last_comp[MAX_FILENAME_LEN+1];

     // Traverse the path to find the file's inode
     // Do not create missing components during extraction
     if (traverse_path(exfs2_path, &file_inode_num, false, last_comp) != 0) {
         // Error message printed by traverse_path
         return;
     }

     // Read the file's inode
     exfs2_inode file_inode;
     if (read_inode(file_inode_num, &file_inode) != 0) {
         fprintf(stderr, "extract_file: Failed to read inode %u for path '%s'\n", file_inode_num, exfs2_path);
         return;
     }

     // Check if it's a regular file
     if (!(file_inode.mode & EXFS2_IFREG)) {
         fprintf(stderr, "extract_file: Error - '%s' (inode %u) is not a regular file.\n", exfs2_path, file_inode_num);
         return;
     }

     char buffer[BLOCK_SIZE];
     uint64_t remaining_size = file_inode.size;
     uint64_t current_offset = 0;
     bool read_error = false;

     // Read blocks sequentially and write to stdout
     while (remaining_size > 0) {
         // Get the block number for the current offset (don't allocate)
         uint32_t current_block_num = get_block_num_for_file_offset(&file_inode, current_offset, false, file_inode_num);

         if (current_block_num == 0 && file_inode.size > 0) { // Allow reading 0-byte file
             fprintf(stderr, "extract_file: Error - Found null block pointer at offset %" PRIu64 " in inode %u (file likely corrupt or sparse file read error)\n", current_offset, file_inode_num);
             read_error = true;
             break;
         } else if (current_block_num == 0 && file_inode.size == 0) {
             // Special case for 0-byte file, nothing to read
             break;
         }


         // Read the data block
         if (read_block(current_block_num, buffer) != 0) {
             fprintf(stderr, "extract_file: Error reading data block %u for inode %u\n", current_block_num, file_inode_num);
             read_error = true;
             break;
         }

         // Determine how much to write from this block
         size_t bytes_to_write = (remaining_size < BLOCK_SIZE) ? (size_t)remaining_size : BLOCK_SIZE;

         // Write to standard output
         if (bytes_to_write > 0) {
            if (fwrite(buffer, 1, bytes_to_write, stdout) != bytes_to_write) {
                perror("extract_file: Error writing to standard output");
                read_error = true;
                break;
            }
         }


         // Update remaining size and offset
         remaining_size -= bytes_to_write;
         current_offset += bytes_to_write;
     }

     // Ensure output is flushed, especially if piping
     fflush(stdout);

     if (read_error) {
         fprintf(stderr, "Extraction of '%s' failed due to errors.\n", exfs2_path);
     }
}

// Recursively frees blocks and potentially inodes starting from a given inode.
// Used for deleting files and directories.
void recursive_free(uint32_t inode_num) {
     // Base case: Invalid inode number (includes 0, which shouldn't be freed this way)
     if (inode_num == 0 || inode_num == UINT32_MAX) {
         fprintf(stderr, "Warning: recursive_free called with invalid inode number %u\n", inode_num);
         return;
     }

     exfs2_inode inode;
     if (read_inode(inode_num, &inode) != 0) {
         fprintf(stderr, "recursive_free: Failed to read inode %u, attempting to free inode entry anyway.\n", inode_num);
         free_inode(inode_num); // Free the inode number from the bitmap regardless
         return;
     }

     // If it's a directory, recursively free its contents first
     if (inode.mode & EXFS2_IFDIR) {
         char block_buf[BLOCK_SIZE];
         // Iterate through direct blocks
         for (int i = 0; i < NUM_DIRECT; ++i) {
             uint32_t block_num = inode.direct_blocks[i];
             if (block_num == 0) continue;

             // Read the directory block
             // If read fails, we cannot recurse into it, but we still need to free the block itself later.
             if (read_block(block_num, block_buf) == 0) {
                 exfs2_dirent *entries = (exfs2_dirent *)block_buf;
                 for (int j = 0; j < DIRENTS_PER_BLOCK; ++j) {
                     uint32_t entry_inode_num = entries[j].inode_num;
                     // Skip unused entries and "." / ".." self-references
                     if (entry_inode_num != 0 && strcmp(entries[j].name, ".") != 0 && strcmp(entries[j].name, "..") != 0) {
                         recursive_free(entry_inode_num); // Recurse!
                         // We don't need to explicitly remove the entry here, as the parent dir block will be freed soon
                     }
                 }
             } else {
                  fprintf(stderr, "Warning: recursive_free: Failed read block %u for dir inode %u during free - cannot free contents\n", block_num, inode_num);
             }
         }
         // TODO: Handle directory contents in indirect blocks
         if (inode.single_indirect != 0) fprintf(stderr, "Warning: recursive_free doesn't handle directory contents in single indirect blocks.\n");
         // ... double/triple ...
     }

     // Now free the data blocks associated with this inode (file or directory)
     // Free direct blocks
     for (int i = 0; i < NUM_DIRECT; ++i) {
         if (inode.direct_blocks[i] != 0) {
             free_block(inode.direct_blocks[i]);
             inode.direct_blocks[i] = 0; // Mark as freed in memory (optional)
         }
     }

     // Free blocks pointed to by single indirect block, then the indirect block itself
     if (inode.single_indirect != 0) {
         indirect_block indir_block;
         // If we can read the indirect block, free the data blocks it points to
         if (read_block(inode.single_indirect, (char *)&indir_block) == 0) {
             for (int i = 0; i < POINTERS_PER_INDIRECT_BLOCK; ++i) {
                 if (indir_block.block_ptrs[i] != 0) {
                     free_block(indir_block.block_ptrs[i]);
                 }
             }
         } else {
             fprintf(stderr, "Warning: recursive_free: Failed read single indirect block %u for inode %u - cannot free data blocks within it.\n", inode.single_indirect, inode_num);
         }
         // Always free the indirect block itself
         free_block(inode.single_indirect);
         inode.single_indirect = 0; // Mark as freed in memory
     }

     // TODO: Free double indirect blocks (free data blocks, then level 1 indirect blocks, then the level 2 block)
     if (inode.double_indirect != 0) {
          fprintf(stderr, "Warning: recursive_free doesn't handle freeing double indirect blocks yet.\n");
          free_block(inode.double_indirect);
     }

     // TODO: Free triple indirect blocks
     if (inode.triple_indirect != 0) {
          fprintf(stderr, "Warning: recursive_free doesn't handle freeing triple indirect blocks yet.\n");
          free_block(inode.triple_indirect);
     }

     // Finally, free the inode itself from the inode bitmap
     free_inode(inode_num);
}

// Removes a file or directory from the ExFS2 filesystem.
void remove_file_or_dir(const char *exfs2_path) {
     // Handle edge cases: cannot remove root "/"
     if (strcmp(exfs2_path, "/") == 0) {
         fprintf(stderr, "remove_file_or_dir: Cannot remove the root directory '/'.\n");
         return;
     }

     char parent_path_buf[1024];
     char target_name_buf[MAX_FILENAME_LEN + 1];
     uint32_t parent_inode_num;
     uint32_t target_inode_num;

     // Separate directory path and target name
     char *path_copy = strdup(exfs2_path);
     if (!path_copy) { perror("strdup"); return; }
     strncpy(parent_path_buf, dirname(path_copy), sizeof(parent_path_buf) - 1);
     parent_path_buf[sizeof(parent_path_buf)-1] = '\0';
     free(path_copy);

     path_copy = strdup(exfs2_path);
     if (!path_copy) { perror("strdup"); return; }
     strncpy(target_name_buf, basename(path_copy), MAX_FILENAME_LEN);
     target_name_buf[MAX_FILENAME_LEN] = '\0';
     free(path_copy);

     // Cannot remove "." or ".." directly
     if (strcmp(target_name_buf, ".") == 0 || strcmp(target_name_buf, "..") == 0) {
         fprintf(stderr, "remove_file_or_dir: Cannot remove '.' or '..'.\n");
         return;
     }

     // Traverse to the parent directory (don't create anything)
      // Handle potential "." or "/" parent path similar to add_file
    if (strcmp(parent_path_buf, "/") == 0 || strcmp(parent_path_buf, ".") == 0) {
        parent_inode_num = ROOT_INODE_NUM;
         printf("remove_file_or_dir: Parent directory is ROOT (inode %u)\n", parent_inode_num);
    } else {
        printf("remove_file_or_dir: Traversing to parent directory '%s'\n", parent_path_buf);
        if (traverse_path(parent_path_buf, &parent_inode_num, false, NULL) != 0) {
            fprintf(stderr, "remove_file_or_dir: Parent path '%s' not found.\n", parent_path_buf);
            return;
        }
         printf("remove_file_or_dir: Resolved parent directory '%s' to inode %u\n", parent_path_buf, parent_inode_num);
    }

     // Find the inode number of the target within the parent directory
     target_inode_num = find_entry_in_dir(parent_inode_num, target_name_buf);
     if (target_inode_num == 0) {
         fprintf(stderr, "remove_file_or_dir: Target '%s' not found in directory (inode %u).\n", target_name_buf, parent_inode_num);
         return;
     }

     // Remove the directory entry from the parent directory first
     if (remove_entry_from_dir(parent_inode_num, target_name_buf) != 0) {
         fprintf(stderr, "remove_file_or_dir: Failed to remove directory entry '%s' from parent inode %u.\n", target_name_buf, parent_inode_num);
         return;
     }

     // Recursively free the target inode and all its associated data blocks
     recursive_free(target_inode_num);

     printf("Successfully removed '%s' (inode %u)\n", exfs2_path, target_inode_num);
}


// Debugging function to trace path resolution and show inode info.
void debug_path(const char* exfs2_path) {
     char *path_copy = strdup(exfs2_path);
     if (!path_copy) { perror("strdup debug_path"); return; }

     char *current_path = path_copy;
     uint32_t current_inode_num = ROOT_INODE_NUM;

     printf("Debugging path resolution for: %s\n", exfs2_path);

     // Print info about root inode
     exfs2_inode current_inode_data; // Declare here for broader scope
     // ***** FIX: Corrected variable name and added & *****
     if(read_inode(ROOT_INODE_NUM, &current_inode_data) == 0) {
          printf("Inode %u (ROOT): mode=%s, size=%" PRIu64 "\n",
                 ROOT_INODE_NUM,
                 (current_inode_data.mode & EXFS2_IFDIR) ? "DIR" : "REG",
                 current_inode_data.size);
          printf("  Direct Blocks: [");
          for(int i=0; i<NUM_DIRECT; ++i) printf(" %u", current_inode_data.direct_blocks[i]);
          printf(" ]\n");
          printf("  Single Indirect: %u\n", current_inode_data.single_indirect);
          // TODO: Print double/triple if implemented
     } else {
         fprintf(stderr, "Debug: Failed read ROOT inode %u\n", ROOT_INODE_NUM);
         free(path_copy);
         return;
     }

     // Handle absolute paths - skip leading slashes
     while (current_path[0] == '/') {
         current_path++;
     }

     // If path was only slashes (or empty after stripping), we are done.
     if (strlen(current_path) == 0) {
         printf("Path resolves to ROOT inode %u\n", ROOT_INODE_NUM);
         free(path_copy);
         return;
     }

     char *token;
     char *rest = current_path;
     char component[MAX_FILENAME_LEN + 1];

     while ((token = strtok_r(rest, "/", &rest))) {
         strncpy(component, token, MAX_FILENAME_LEN);
         component[MAX_FILENAME_LEN] = '\0';

         printf("--> Searching for component: '%s' in directory inode %u\n", component, current_inode_num);

         // Read current directory inode (needed to check it's a dir before searching)
         // ***** FIX: Corrected variable name and added & *****
         if (read_inode(current_inode_num, &current_inode_data) != 0) {
             fprintf(stderr, "Debug: Failed read current directory inode %u before searching for '%s'\n", current_inode_num, component);
             break; // Cannot continue
         }
         if (!(current_inode_data.mode & EXFS2_IFDIR)) {
             fprintf(stderr, "Debug: Error - inode %u is not a directory, cannot search for '%s'\n", current_inode_num, component);
             break; // Cannot continue
         }

         // Find the entry in the current directory
         uint32_t next_inode_num = find_entry_in_dir(current_inode_num, component);

         if (next_inode_num == 0) {
             printf("  Component '%s' NOT FOUND in directory inode %u\n", component, current_inode_num);
             break; // Path resolution fails here
         }

         // Found the component, print info about the found inode
         printf("  Found '%s' -> maps to inode %u\n", component, next_inode_num);
         current_inode_num = next_inode_num; // Move to the next inode

         // Read and display info about the found inode
         // ***** FIX: Corrected variable name and added & *****
         if (read_inode(current_inode_num, &current_inode_data) == 0) {
             printf("Inode %u: mode=%s, size=%" PRIu64 "\n",
                    current_inode_num,
                    (current_inode_data.mode & EXFS2_IFDIR) ? "DIR" : "REG",
                    current_inode_data.size);
             printf("  Direct Blocks: [");
             for(int i=0; i<NUM_DIRECT; ++i) printf(" %u", current_inode_data.direct_blocks[i]);
             printf(" ]\n");
             printf("  Single Indirect: %u\n", current_inode_data.single_indirect);
             // TODO: Print double/triple if implemented
         } else {
             fprintf(stderr, "Debug: Failed read inode %u (for component '%s')\n", current_inode_num, component);
         }

         // Check if this was the last component
         if (rest == NULL || strlen(rest) == 0) {
             printf("Path resolution finished at inode %u\n", current_inode_num);
             break;
         }
     } // End while loop over tokens

     free(path_copy);
 }


// --- Main Function ---
void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s <operation> [arguments...]\n", prog_name);
    fprintf(stderr, "Operations:\n");
    fprintf(stderr, "  --ls <exfs2_path>       List directory contents recursively\n");
    fprintf(stderr, "  --add <local_path> <exfs2_path> Add a local file to the filesystem\n");
    fprintf(stderr, "  --cat <exfs2_path>      Extract file contents to standard output\n");
    fprintf(stderr, "  --rm <exfs2_path>       Remove a file or directory recursively\n");
    fprintf(stderr, "  --debug <exfs2_path>    Debug path traversal and inode details\n");
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Initialize filesystem structure (ensure segments exist)
    // This happens automatically on every run now.
    initialize_exfs2();

    // Use standard getopt_long for clearer argument parsing
    int opt = 0;
    int long_index = 0;
    const char *op_ls_path = NULL;
    const char *op_add_local = NULL;
    const char *op_add_exfs2 = NULL;
    const char *op_cat_path = NULL;
    const char *op_rm_path = NULL;
    const char *op_debug_path = NULL;

    // Define long options (removed init)
    static struct option long_options[] = {
        {"ls",    required_argument, 0, 'l'},
        {"add",   required_argument, 0, 'a'}, // Expects 2 args after this
        {"cat",   required_argument, 0, 'c'},
        {"rm",    required_argument, 0, 'r'},
        {"debug", required_argument, 0, 'd'},
        {0,       0,                 0,  0 }
    };

    // Basic parsing to identify the main operation
    // Using "+" at start of optstring to prevent permutation
    // Removed 'i' from optstring
    while ((opt = getopt_long(argc, argv, "+l:a:c:r:d:", long_options, &long_index)) != -1) {
        switch (opt) {
            case 'l': op_ls_path = optarg; break;
            case 'a':
                op_add_local = optarg;
                // Check if next argument exists for the exfs2 path
                if (optind < argc) {
                    op_add_exfs2 = argv[optind];
                    optind++; // Consume the second argument for add
                } else {
                    fprintf(stderr, "Error: --add requires <local_path> and <exfs2_path>\n");
                    print_usage(argv[0]);
                    return EXIT_FAILURE;
                }
                break;
            case 'c': op_cat_path = optarg; break;
            case 'r': op_rm_path = optarg; break;
            case 'd': op_debug_path = optarg; break;
            case '?': // Unknown option or missing argument
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
         // Allow only one operation at a time for simplicity
        if (opt != '?') { // Don't break on error, let it exit
            break;
        }
    }

     // Check if any operation was actually selected (removed op_init)
    int ops_count = (op_ls_path ? 1 : 0) + (op_add_local ? 1 : 0) +
                    (op_cat_path ? 1 : 0) + (op_rm_path ? 1 : 0) + (op_debug_path ? 1 : 0);

    if (ops_count == 0) {
        if (optind < argc) {
             fprintf(stderr, "Error: Unknown operation or extra arguments: %s\n", argv[optind]);
        } else {
             fprintf(stderr, "Error: No operation specified.\n");
        }
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (ops_count > 1) {
         fprintf(stderr, "Error: Only one operation can be specified at a time.\n");
         print_usage(argv[0]);
         return EXIT_FAILURE;
    }

    // --- Execute the chosen operation ---

    if (op_ls_path) {
        printf("Listing directory: %s\n", op_ls_path);
        uint32_t target_inode;
        char last_comp[MAX_FILENAME_LEN + 1];
        if (traverse_path(op_ls_path, &target_inode, false, last_comp) == 0) {
             // Check if the target is actually a directory before listing
             exfs2_inode target_inode_data;
             if (read_inode(target_inode, &target_inode_data) == 0) {
                 if (target_inode_data.mode & EXFS2_IFDIR) {
                    list_directory(target_inode, 0);
                 } else {
                     fprintf(stderr, "ls: '%s' is not a directory\n", op_ls_path);
                     return EXIT_FAILURE;
                 }
             } else {
                 fprintf(stderr, "ls: Failed to read inode %u for '%s'\n", target_inode, op_ls_path);
                 return EXIT_FAILURE;
             }
        } else {
             fprintf(stderr, "ls: Cannot access '%s': No such file or directory\n", op_ls_path);
             return EXIT_FAILURE;
        }
    } else if (op_add_local && op_add_exfs2) {
        add_file(op_add_exfs2, op_add_local);
    } else if (op_cat_path) {
        extract_file(op_cat_path);
    } else if (op_rm_path) {
        remove_file_or_dir(op_rm_path);
    } else if (op_debug_path) {
        debug_path(op_debug_path);
    } else {
        // Should not be reachable if ops_count logic is correct
        fprintf(stderr, "Internal error: No operation selected for execution.\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}