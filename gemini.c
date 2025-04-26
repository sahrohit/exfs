#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h> // For bool type

// --- Constants ---

#define BLOCK_SIZE 4096            // Standard block size
#define SEGMENT_SIZE (1024 * 1024) // 1MB segment size

// Inode Segment Constants
#define INODE_BITMAP_BLOCKS 1                                      // User request: first block (4096 bytes) for bitmap
#define INODES_PER_SEGMENT 255                                     // Calculated: (SEGMENT_SIZE - INODE_BITMAP_BLOCKS * BLOCK_SIZE) / BLOCK_SIZE
#define INODE_BITMAP_SIZE_BITS INODES_PER_SEGMENT                  // One bit per inode
#define INODE_BITMAP_SIZE_BYTES ((INODE_BITMAP_SIZE_BITS + 7) / 8) // Bytes needed for the bitmap

// Data Segment Constants
#define DATA_BITMAP_BLOCKS 1                                     // User request: first block (4096 bytes) for bitmap
#define DATA_BLOCKS_PER_SEGMENT 255                              // Calculated: (SEGMENT_SIZE - DATA_BITMAP_BLOCKS * BLOCK_SIZE) / BLOCK_SIZE
#define DATA_BITMAP_SIZE_BITS DATA_BLOCKS_PER_SEGMENT            // One bit per data block
#define DATA_BITMAP_SIZE_BYTES ((DATA_BITMAP_SIZE_BITS + 7) / 8) // Bytes needed for the bitmap

#define MAX_FILENAME_LEN 255
#define MAX_DIRENTS_PER_BLOCK (BLOCK_SIZE / sizeof(dirent_t)) // Calculate how many dirents fit in a block

// File types for inode
#define TYPE_REGULAR_FILE 1
#define TYPE_DIRECTORY 2
#define TYPE_FREE 0 // Indicates a free inode slot

// Calculate MAX_DIRECT_BLOCKS based on inode_t size minus other fields
// Assuming block pointers are uint32_t (4 bytes)
#define INODE_METADATA_SIZE (sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t)) // type, size, single_indirect, double_indirect
#define MAX_DIRECT_BLOCKS ((BLOCK_SIZE - INODE_METADATA_SIZE) / sizeof(uint32_t))

// --- Struct Definitions ---

// Inode Structure (as provided by user, adjusted for consistency)
typedef struct
{
    uint32_t type;                             // File type (TYPE_REGULAR_FILE, TYPE_DIRECTORY, TYPE_FREE) implicitly needed
    uint64_t size;                             // File size in bytes
    uint32_t direct_blocks[MAX_DIRECT_BLOCKS]; // Direct block pointers
    uint32_t single_indirect;                  // Single indirect block pointer
    uint32_t double_indirect;                  // Double indirect block pointer
    // Triple indirect mentioned in PDF but not in user struct, omitted per user struct.
} inode_t; // Total size should be <= BLOCK_SIZE

// Data Block Structure (as provided by user)
typedef struct
{
    char data[BLOCK_SIZE];
} datablock_t;

// Directory Entry Structure (as provided by user)
typedef struct
{
    char name[MAX_FILENAME_LEN]; // File or directory name
    uint32_t nodenum;            // Inode number for this entry
    // uint32_t type;          // Redundant? Inode has type. Could be useful for quick checks.
    // uint32_t inuse;         // Useful to mark deleted entries without rewriting block
} dirent_t;

// Structure representing a block containing directory entries
typedef struct
{
    dirent_t dirents[MAX_DIRENTS_PER_BLOCK];
} dirblock_t; // Should fit in one datablock_t

// --- Global Variables (Illustrative - manage file handles appropriately) ---
// You'll need a more robust way to manage potentially many segment files.
// This could involve dynamically tracking open segments.
// int current_inode_segment_fd = -1; // Example - better to pass fds or use a manager struct
// int current_data_segment_fd = -1;
// int next_inode_segment_id = 0;
// int next_data_segment_id = 0;

// --- Function Prototypes ---

// Bitmap manipulation helpers
void set_bit(unsigned char *bitmap, int n);
void clear_bit(unsigned char *bitmap, int n);
bool get_bit(const unsigned char *bitmap, int n);

// Segment management
int open_or_create_inode_segment(int segment_id);
int open_or_create_data_segment(int segment_id);
void initialize_new_inode_segment(int fd);
void initialize_new_data_segment(int fd);

// Inode operations
int find_free_inode(uint32_t *segment_id, uint32_t *inode_index_in_segment);
int read_inode(uint32_t segment_id, uint32_t inode_index, inode_t *inode);
int write_inode(uint32_t segment_id, uint32_t inode_index, const inode_t *inode);
int allocate_inode(uint32_t *segment_id, uint32_t *inode_index_in_segment);
void free_inode(uint32_t segment_id, uint32_t inode_index_in_segment);

// Data block operations
int find_free_data_block(uint32_t *segment_id, uint32_t *block_index_in_segment);
int read_data_block(uint32_t segment_id, uint32_t block_index, datablock_t *block);
int write_data_block(uint32_t segment_id, uint32_t block_index, const datablock_t *block);
int allocate_data_block(uint32_t *segment_id, uint32_t *block_index_in_segment);
void free_data_block(uint32_t segment_id, uint32_t block_index_in_segment);

// File system initialization
void initialize_filesystem();

// --- Function Implementations ---

/* ... (Implementations for bitmap, segment management, inode ops, data block ops as before) ... */
// --- Bitmap Functions ---
void set_bit(unsigned char *bitmap, int n)
{
    bitmap[n / 8] |= (1 << (n % 8));
}

void clear_bit(unsigned char *bitmap, int n)
{
    bitmap[n / 8] &= ~(1 << (n % 8));
}

bool get_bit(const unsigned char *bitmap, int n)
{
    return (bitmap[n / 8] & (1 << (n % 8))) != 0;
}

// --- Segment Management ---
// Creates segment file if it doesn't exist, initializes if new. Returns fd.
int open_or_create_inode_segment(int segment_id)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "inodeseg%d", segment_id);
    // Check if file exists
    struct stat st;
    bool exists = (stat(filename, &st) == 0);

    int fd = open(filename, O_RDWR | O_CREAT, 0666); // Create if doesn't exist
    if (fd < 0)
    {
        perror("Error opening/creating inode segment");
        return -1;
    }

    if (!exists || st.st_size < SEGMENT_SIZE)
    {
        // New file or incomplete file, initialize/truncate
        if (ftruncate(fd, SEGMENT_SIZE) == -1)
        {
            perror("Error truncating inode segment");
            close(fd);
            return -1;
        }
        initialize_new_inode_segment(fd);
        // Root directory inode initialization moved to initialize_filesystem
    }
    // printf("Opened inode segment %d (fd: %d)\n", segment_id, fd); // Less verbose
    return fd;
}

int open_or_create_data_segment(int segment_id)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "dataseg%d", segment_id);
    struct stat st;
    bool exists = (stat(filename, &st) == 0);

    int fd = open(filename, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
    {
        perror("Error opening/creating data segment");
        return -1;
    }

    if (!exists || st.st_size < SEGMENT_SIZE)
    {
        if (ftruncate(fd, SEGMENT_SIZE) == -1)
        {
            perror("Error truncating data segment");
            close(fd);
            return -1;
        }
        initialize_new_data_segment(fd);
    }
    // printf("Opened data segment %d (fd: %d)\n", segment_id, fd); // Less verbose
    return fd;
}

void initialize_new_inode_segment(int fd)
{
    // printf("Initializing new inode segment (fd: %d)\n", fd); // Less verbose
    // 1. Zero out the bitmap block
    unsigned char local_bitmap_block[BLOCK_SIZE]; // Use local variable
    memset(local_bitmap_block, 0, BLOCK_SIZE);
    if (pwrite(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error writing initial inode bitmap block");
        // Handle error - maybe close fd and return error?
    }
    // 2. Optionally zero out inode blocks (or rely on allocation logic)
    // For simplicity, we might not zero them here, but ensure allocate_inode initializes.
}

void initialize_new_data_segment(int fd)
{
    // printf("Initializing new data segment (fd: %d)\n", fd); // Less verbose
    // 1. Zero out the bitmap block
    unsigned char local_bitmap_block[BLOCK_SIZE]; // Use local variable
    memset(local_bitmap_block, 0, BLOCK_SIZE);
    if (pwrite(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error writing initial data bitmap block");
        // Handle error
    }
    // 2. No need to zero data blocks implicitly
}

// --- Inode Operations ---
int find_free_inode(uint32_t *segment_id, uint32_t *inode_index_in_segment)
{
    unsigned char local_bitmap_block[BLOCK_SIZE]; // Buffer for the bitmap
    int current_seg_id = 0;

    while (true)
    { // Loop through segments
        int fd = open_or_create_inode_segment(current_seg_id);
        if (fd < 0)
            return -1; // Error opening/creating

        // Read the bitmap (first block)
        if (pread(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
        {
            perror("Error reading inode bitmap");
            close(fd); // Close on error
            return -1;
        }

        // Search the bitmap for a 0 bit
        for (int i = 0; i < INODES_PER_SEGMENT; ++i)
        {
            if (!get_bit(local_bitmap_block, i))
            {
                *segment_id = current_seg_id;
                *inode_index_in_segment = i;
                // printf("Found free inode: seg=%u, index=%u\n", *segment_id, *inode_index_in_segment); // Less verbose
                // Keep fd open if needed, or close if managed elsewhere
                close(fd);
                return 0; // Success
            }
        }
        close(fd); // Close current segment fd before trying next
        current_seg_id++;
        // Check for reasonable limit? Or rely on disk space.
        // printf("Inode segment %d full, trying next...\n", current_seg_id -1 ); // Less verbose
        // Implicit creation of next segment happens in next loop iteration's open_or_create
    }
    // Should not be reached if segments can always be created
    return -1; // No free inode found (should theoretically not happen if space exists)
}

int read_inode(uint32_t segment_id, uint32_t inode_index, inode_t *inode)
{
    if (inode_index >= INODES_PER_SEGMENT)
        return -1; // Invalid index

    int fd = open_or_create_inode_segment(segment_id); // Ensure segment is open
    if (fd < 0)
        return -1;

    off_t offset = (INODE_BITMAP_BLOCKS + inode_index) * BLOCK_SIZE;
    if (pread(fd, inode, sizeof(inode_t), offset) != sizeof(inode_t))
    {
        // Check if read less than expected, could indicate issue
        // Might return 0 (EOF) if reading past end of *partially* written file? Unlikely with ftruncate
        // perror("Error reading inode"); // Suppress error if it might be first read
        close(fd);
        return -1; // Indicate error or not found
    }
    // Remember inode_t might be smaller than BLOCK_SIZE, reading BLOCK_SIZE might be safer
    // if padding exists or structure changes. For now, read exact struct size.

    close(fd); // Close after operation
    return 0;
}

int write_inode(uint32_t segment_id, uint32_t inode_index, const inode_t *inode)
{
    if (inode_index >= INODES_PER_SEGMENT)
        return -1;

    int fd = open_or_create_inode_segment(segment_id);
    if (fd < 0)
        return -1;

    off_t offset = (INODE_BITMAP_BLOCKS + inode_index) * BLOCK_SIZE;
    // Create a full block buffer to ensure we write exactly BLOCK_SIZE
    char inode_block_buffer[BLOCK_SIZE];                // Renamed buffer variable
    memset(inode_block_buffer, 0, BLOCK_SIZE);          // Zero out the block first
    memcpy(inode_block_buffer, inode, sizeof(inode_t)); // Copy inode data

    if (pwrite(fd, inode_block_buffer, BLOCK_SIZE, offset) != BLOCK_SIZE)
    {
        perror("Error writing inode");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int allocate_inode(uint32_t *segment_id, uint32_t *inode_index_in_segment)
{
    if (find_free_inode(segment_id, inode_index_in_segment) != 0)
    {
        fprintf(stderr, "Failed to find a free inode.\n");
        return -1;
    }

    // Mark inode as used in the bitmap
    int fd = open_or_create_inode_segment(*segment_id);
    if (fd < 0)
        return -1;

    unsigned char local_bitmap_block[BLOCK_SIZE]; // Use local variable
    if (pread(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error reading inode bitmap for allocation");
        close(fd);
        return -1;
    }

    if (get_bit(local_bitmap_block, *inode_index_in_segment))
    {
        fprintf(stderr, "Error: Inode %u in segment %u already allocated.\n", *inode_index_in_segment, *segment_id);
        close(fd);
        return -1; // Should not happen if find_free_inode is correct
    }

    set_bit(local_bitmap_block, *inode_index_in_segment);

    if (pwrite(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error writing inode bitmap after allocation");
        // Attempt to revert? Or just report error.
        close(fd);
        return -1;
    }

    // Initialize the inode structure on disk
    inode_t new_inode;
    memset(&new_inode, 0, sizeof(inode_t));
    new_inode.type = TYPE_FREE; // Mark as free until type is set by caller
    new_inode.size = 0;
    // No need to zero block pointers explicitly due to memset

    // Close fd before calling write_inode, as it opens/closes its own fd
    close(fd);

    if (write_inode(*segment_id, *inode_index_in_segment, &new_inode) != 0)
    {
        fprintf(stderr, "Failed to write initial empty inode.\n");
        // Attempt to clear bit in bitmap? Requires reopening fd, reading, clearing, writing bitmap.
        // For now, just report error.
        return -1;
    }

    // printf("Allocated inode: seg=%u, index=%u\n", *segment_id, *inode_index_in_segment); // Less verbose
    return 0; // Success
}

void free_inode(uint32_t segment_id, uint32_t inode_index_in_segment)
{
    if (inode_index_in_segment >= INODES_PER_SEGMENT)
        return;

    int fd = open_or_create_inode_segment(segment_id);
    if (fd < 0)
        return;

    // 1. Clear the bit in the inode bitmap
    unsigned char local_bitmap_block[BLOCK_SIZE]; // Use local variable
    if (pread(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error reading inode bitmap for freeing");
        close(fd);
        return;
    }

    clear_bit(local_bitmap_block, inode_index_in_segment);

    if (pwrite(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error writing inode bitmap after freeing");
        // Bitmap might be inconsistent now.
    }

    // 2. Optionally clear the inode block itself (or just rely on the bitmap)
    // For safety, clearing the type might be good.
    inode_t cleared_inode;
    memset(&cleared_inode, 0, sizeof(inode_t));
    cleared_inode.type = TYPE_FREE;

    // Close fd before calling write_inode
    close(fd);

    write_inode(segment_id, inode_index_in_segment, &cleared_inode); // Ignore write error for now

    // printf("Freed inode: seg=%u, index=%u\n", segment_id, inode_index_in_segment); // Less verbose
    // Note: This does NOT free the data blocks associated with the inode.
    // That requires reading the inode *before* freeing it and freeing its blocks.
}

// --- Data Block Operations --- (Similar logic to inode ops)

int find_free_data_block(uint32_t *segment_id, uint32_t *block_index_in_segment)
{
    unsigned char local_bitmap_block[BLOCK_SIZE]; // Use local variable
    int current_seg_id = 0;

    while (true)
    { // Loop through segments
        int fd = open_or_create_data_segment(current_seg_id);
        if (fd < 0)
            return -1;

        if (pread(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
        {
            perror("Error reading data bitmap");
            close(fd);
            return -1;
        }

        for (int i = 0; i < DATA_BLOCKS_PER_SEGMENT; ++i)
        {
            if (!get_bit(local_bitmap_block, i))
            {
                *segment_id = current_seg_id;
                *block_index_in_segment = i;
                // printf("Found free data block: seg=%u, index=%u\n", *segment_id, *block_index_in_segment); // Less verbose
                close(fd);
                return 0; // Success
            }
        }
        close(fd);
        current_seg_id++;
        // printf("Data segment %d full, trying next...\n", current_seg_id - 1); // Less verbose
        // Implicit creation in next loop via open_or_create
    }
    return -1; // Should not be reached
}

int read_data_block(uint32_t segment_id, uint32_t block_index, datablock_t *block)
{
    if (block_index >= DATA_BLOCKS_PER_SEGMENT)
        return -1;

    int fd = open_or_create_data_segment(segment_id);
    if (fd < 0)
        return -1;

    off_t offset = (DATA_BITMAP_BLOCKS + block_index) * BLOCK_SIZE;
    if (pread(fd, block, sizeof(datablock_t), offset) != sizeof(datablock_t))
    {
        perror("Error reading data block");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int write_data_block(uint32_t segment_id, uint32_t block_index, const datablock_t *block)
{
    if (block_index >= DATA_BLOCKS_PER_SEGMENT)
        return -1;

    int fd = open_or_create_data_segment(segment_id);
    if (fd < 0)
        return -1;

    off_t offset = (DATA_BITMAP_BLOCKS + block_index) * BLOCK_SIZE;
    if (pwrite(fd, block, sizeof(datablock_t), offset) != sizeof(datablock_t))
    {
        perror("Error writing data block");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int allocate_data_block(uint32_t *segment_id, uint32_t *block_index_in_segment)
{
    if (find_free_data_block(segment_id, block_index_in_segment) != 0)
    {
        fprintf(stderr, "Failed to find a free data block.\n");
        return -1;
    }

    int fd = open_or_create_data_segment(*segment_id);
    if (fd < 0)
        return -1;

    unsigned char local_bitmap_block[BLOCK_SIZE]; // Use local variable
    if (pread(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error reading data bitmap for allocation");
        close(fd);
        return -1;
    }

    if (get_bit(local_bitmap_block, *block_index_in_segment))
    {
        fprintf(stderr, "Error: Data block %u in segment %u already allocated.\n", *block_index_in_segment, *segment_id);
        close(fd);
        return -1;
    }

    set_bit(local_bitmap_block, *block_index_in_segment);

    if (pwrite(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error writing data bitmap after allocation");
        close(fd);
        return -1;
    }

    // Optional: Zero out the allocated block? Not strictly required by PDF
    // datablock_t empty_block;
    // memset(&empty_block, 0, sizeof(datablock_t));
    // close(fd); // Close before calling write_data_block
    // write_data_block(*segment_id, *block_index_in_segment, &empty_block);

    // printf("Allocated data block: seg=%u, index=%u\n", *segment_id, *block_index_in_segment); // Less verbose
    close(fd); // Close fd if not zeroing block
    return 0;  // Success
}

void free_data_block(uint32_t segment_id, uint32_t block_index_in_segment)
{
    if (block_index_in_segment >= DATA_BLOCKS_PER_SEGMENT)
        return;

    int fd = open_or_create_data_segment(segment_id);
    if (fd < 0)
        return;

    unsigned char local_bitmap_block[BLOCK_SIZE]; // Use local variable
    if (pread(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error reading data bitmap for freeing");
        close(fd);
        return;
    }

    clear_bit(local_bitmap_block, block_index_in_segment);

    if (pwrite(fd, local_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Error writing data bitmap after freeing");
    }

    // No need to clear the actual data block content
    // printf("Freed data block: seg=%u, index=%u\n", segment_id, block_index_in_segment); // Less verbose
    close(fd);
}

// --- Filesystem Initialization ---
// ** CORRECTED FUNCTION **
void initialize_filesystem()
{
    printf("Initializing ExFS2...\n");

    // 1. Ensure inode segment 0 exists and get fd
    int inode_fd = open_or_create_inode_segment(0);
    if (inode_fd < 0)
    {
        fprintf(stderr, "Failed to initialize inode segment 0.\n");
        exit(1);
    }

    // 2. Read the inode bitmap from segment 0
    unsigned char inode_bitmap_block[BLOCK_SIZE]; // Declare local bitmap buffer
    if (pread(inode_fd, inode_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
    {
        perror("Failed to read inode bitmap from segment 0 during initialization");
        close(inode_fd);
        exit(1);
    }

    // 3. Check if root inode (index 0) is allocated using the bitmap we just read
    if (!get_bit(inode_bitmap_block, 0))
    { // Check bit 0 for inode 0
        printf("Root inode (0,0) appears free. Allocating...\n");

        // Mark inode 0 as allocated IN MEMORY bitmap first
        set_bit(inode_bitmap_block, 0);
        // Write the updated bitmap back to disk
        if (pwrite(inode_fd, inode_bitmap_block, BLOCK_SIZE, 0) != BLOCK_SIZE)
        {
            perror("Critical error: Failed to write updated inode bitmap for root allocation");
            close(inode_fd);
            exit(1);
        }

        // Now allocate a data block for the root directory's entries
        uint32_t root_data_seg, root_data_idx;
        // Note: allocate_data_block opens/closes its own FDs internally now
        if (allocate_data_block(&root_data_seg, &root_data_idx) != 0)
        {
            fprintf(stderr, "Critical error: Failed to allocate data block for root directory.\n");
            // Need to revert the inode bitmap change
            clear_bit(inode_bitmap_block, 0);                    // Clear in memory
            pwrite(inode_fd, inode_bitmap_block, BLOCK_SIZE, 0); // Write back cleared bitmap
            close(inode_fd);
            exit(1);
        }

        // Initialize root inode structure
        inode_t root_inode;
        memset(&root_inode, 0, sizeof(inode_t));
        root_inode.type = TYPE_DIRECTORY;
        root_inode.size = 0; // Initially empty directory

        // Combine segment and index for storing block pointer.
        // Example: Using upper 16 bits for segment, lower 16 for index. Needs careful masking/shifting on read.
        // Or, use two uint32_t fields in inode if simpler. Sticking to example for now.
        // WARNING: This limits segment ID and index to 16 bits (65535). Choose a robust scheme.
        if (root_data_seg > 0xFFFF || root_data_idx > 0xFFFF)
        {
            fprintf(stderr, "Critical Error: Root data block segment/index too large for simple packing.\n");
            // Clean up allocated data block and inode bit
            free_data_block(root_data_seg, root_data_idx);
            clear_bit(inode_bitmap_block, 0);
            pwrite(inode_fd, inode_bitmap_block, BLOCK_SIZE, 0);
            close(inode_fd);
            exit(1);
        }
        root_inode.direct_blocks[0] = (root_data_seg << 16) | root_data_idx;

        // Initialize the allocated data block for the root directory (zero it out)
        dirblock_t empty_dir_block;
        memset(&empty_dir_block, 0, sizeof(dirblock_t));
        // Note: write_data_block and write_inode open/close their own FDs
        if (write_data_block(root_data_seg, root_data_idx, (datablock_t *)&empty_dir_block) != 0)
        {
            fprintf(stderr, "Critical error: Failed to write initial root directory data block.\n");
            // Clean up allocated data block and inode bit
            free_data_block(root_data_seg, root_data_idx);
            clear_bit(inode_bitmap_block, 0);
            pwrite(inode_fd, inode_bitmap_block, BLOCK_SIZE, 0);
            close(inode_fd);
            exit(1);
        }

        // Write the initialized root inode structure to disk (inode 0, seg 0)
        if (write_inode(0, 0, &root_inode) != 0)
        {
            fprintf(stderr, "Critical error: Failed to write initialized root inode.\n");
            // Clean up allocated data block and inode bit (best effort)
            free_data_block(root_data_seg, root_data_idx);
            clear_bit(inode_bitmap_block, 0);
            pwrite(inode_fd, inode_bitmap_block, BLOCK_SIZE, 0); // Try to write back cleared bitmap
            close(inode_fd);
            exit(1);
        }
        printf("Successfully allocated and initialized root inode (0,0) and its first data block (%u,%u).\n", root_data_seg, root_data_idx);
    }
    else
    {
        printf("Root inode (0,0) already allocated.\n");
    }

    // 4. Close the inode segment 0 fd
    close(inode_fd);

    // 5. Ensure data segment 0 exists
    int data_fd = open_or_create_data_segment(0);
    if (data_fd < 0)
    {
        fprintf(stderr, "Failed to initialize data segment 0.\n");
        // Filesystem might be inconsistent if root inode was just created.
        exit(1);
    }
    close(data_fd);

    printf("Filesystem initialization complete.\n");
}

// --- Main Function (Example Usage Placeholder) ---
int main(int argc, char *argv[])
{
    // Parse command line arguments (e.g., -a, -r, -l, -e)
    // Based on arguments, call appropriate functions (add_file, read_file, remove_file, list_files)

    initialize_filesystem();

    // Example: Allocate an inode and a data block (can be removed for final version)
    uint32_t test_inode_seg, test_inode_idx;
    printf("\nAttempting test allocation...\n");
    if (allocate_inode(&test_inode_seg, &test_inode_idx) == 0)
    {
        printf("Successfully allocated test inode: seg=%u, index=%u\n", test_inode_seg, test_inode_idx);
        // Now you could write to this inode, e.g., set its type and size.
        // Remember to skip inode 0 if it's the root.
        if (test_inode_seg != 0 || test_inode_idx != 0)
        {
            // ... set up and write inode ...
            printf("Freeing test inode (%u,%u)\n", test_inode_seg, test_inode_idx);
            free_inode(test_inode_seg, test_inode_idx); // Clean up test
        }
        else
        {
            printf("Test allocation got root inode (0,0), not freeing.\n");
        }
    }
    else
    {
        printf("Test inode allocation failed.\n");
    }

    uint32_t test_data_seg, test_data_idx;
    if (allocate_data_block(&test_data_seg, &test_data_idx) == 0)
    {
        printf("Successfully allocated test data block: seg=%u, index=%u\n", test_data_seg, test_data_idx);
        // Now you could write data to this block.
        printf("Freeing test data block (%u,%u)\n", test_data_seg, test_data_idx);
        free_data_block(test_data_seg, test_data_idx); // Clean up test
    }
    else
    {
        printf("Test data block allocation failed.\n");
    }

    printf("\nExiting.\n");
    return 0;
}