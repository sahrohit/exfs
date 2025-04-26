#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SEGMENT_SIZE (1024 * 1024) // 1MB segments
#define BLOCK_SIZE 4096            // 4KB blocks
#define INODE_SIZE BLOCK_SIZE      // Each inode is one block

#define MAX_DIRECT_BLOCKS ((INODE_SIZE - 128) / sizeof(uint32_t)) // Rough estimate, adjust based on attributes

// Define inode file index structure and bitmap
#define MAX_INODES (SEGMENT_SIZE / INODE_SIZE) // Maximum inodes in 1MB
#define BITMAP_BYTES (MAX_INODES / 8)          // Size of bitmap in bytes

/* File types */
#define FILE_TYPE_REGULAR 1
#define FILE_TYPE_DIRECTORY 2

typedef struct
{
    uint32_t type;                             // File type (regular or directory)
    uint64_t size;                             // File size in bytes
    uint32_t direct_blocks[MAX_DIRECT_BLOCKS]; // Direct block pointers
    uint32_t single_indirect;                  // Single indirect block
    uint32_t double_indirect;                  // Double indirect block
    uint32_t triple_indirect;                  // Triple indirect block
    // Add other necessary fields here
} inode_t;

/**
 * Initialize a new inode with default values
 *
 * @param inode Pointer to inode structure to initialize
 * @param type Type of file (regular or directory)
 * @param size Initial size of the file in bytes
 */
void init_inode(inode_t *inode, uint32_t type, uint64_t size)
{
    if (inode == NULL)
        return;

    inode->type = type;
    inode->size = size;

    // Initialize direct blocks
    for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
    {
        inode->direct_blocks[i] = i + 1; // Assign sequential block numbers
    }

    // Initialize indirect blocks
    inode->single_indirect = 0;
    inode->double_indirect = 0;
    inode->triple_indirect = 0;
}

/**
 * Write inode segment to file
 *
 * @param filename Name of the file to write to
 * @param next Next available inode index
 * @param bitmap Bitmap array indicating which inodes are in use
 * @param inode Pointer to inode structure to write
 * @param inode_index Index of the inode to write
 * @return 0 on success, -1 on failure
 */
int write_inode(const char *filename, uint32_t next, uint8_t *bitmap, inode_t *inode, uint32_t inode_index)
{
    if (filename == NULL || bitmap == NULL || inode == NULL)
    {
        return -1;
    }

    // Mark the inode as used in the bitmap
    uint32_t byte_index = inode_index / 8;
    uint8_t bit_position = inode_index % 8;
    bitmap[byte_index] |= (1 << bit_position);

    FILE *file = fopen(filename, "wb");
    if (file == NULL)
    {
        perror("Failed to open file for writing");
        return -1;
    }

    // Write next inode index
    if (fwrite(&next, sizeof(next), 1, file) != 1)
    {
        perror("Failed to write next index");
        fclose(file);
        return -1;
    }

    // Write bitmap
    if (fwrite(bitmap, BITMAP_BYTES, 1, file) != 1)
    {
        perror("Failed to write bitmap");
        fclose(file);
        return -1;
    }

    // Seek to the appropriate position for the inode
    long inode_position = sizeof(next) + BITMAP_BYTES + (inode_index * sizeof(inode_t));
    if (fseek(file, inode_position, SEEK_SET) != 0)
    {
        perror("Failed to seek to inode position");
        fclose(file);
        return -1;
    }

    // Write the inode
    if (fwrite(inode, sizeof(inode_t), 1, file) != 1)
    {
        perror("Failed to write inode");
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

/**
 * Read inode segment from file
 *
 * @param filename Name of the file to read from
 * @param next Pointer to store the next available inode index
 * @param bitmap Bitmap array to store which inodes are in use
 * @param inode Pointer to inode structure to read into
 * @param inode_index Index of the inode to read
 * @return 0 on success, -1 on failure
 */
int read_inode(const char *filename, uint32_t *next, uint8_t *bitmap, inode_t *inode, uint32_t inode_index)
{
    if (filename == NULL || next == NULL || bitmap == NULL || inode == NULL)
    {
        return -1;
    }

    FILE *file = fopen(filename, "rb");
    if (file == NULL)
    {
        perror("Failed to open file for reading");
        return -1;
    }

    // Read next inode index
    if (fread(next, sizeof(*next), 1, file) != 1)
    {
        perror("Failed to read next index");
        fclose(file);
        return -1;
    }

    // Read bitmap
    if (fread(bitmap, BITMAP_BYTES, 1, file) != 1)
    {
        perror("Failed to read bitmap");
        fclose(file);
        return -1;
    }

    // Seek to the appropriate position for the inode
    long inode_position = sizeof(*next) + BITMAP_BYTES + (inode_index * sizeof(inode_t));
    if (fseek(file, inode_position, SEEK_SET) != 0)
    {
        perror("Failed to seek to inode position");
        fclose(file);
        return -1;
    }

    // Read the inode
    if (fread(inode, sizeof(inode_t), 1, file) != 1)
    {
        perror("Failed to read inode");
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

/**
 * Print inode information for debugging
 *
 * @param inode Pointer to inode structure to print
 */
void print_inode(inode_t *inode)
{
    if (inode == NULL)
        return;

    printf("Inode Information:\n");
    printf("Type: %u\n", inode->type);
    printf("Size: %lu bytes\n", inode->size);

    printf("Direct blocks: ");
    for (int i = 0; i < 5 && i < MAX_DIRECT_BLOCKS; i++)
    { // Print first 5 blocks for brevity
        printf("%u ", inode->direct_blocks[i]);
    }
    printf("...\n");

    printf("Single indirect: %u\n", inode->single_indirect);
    printf("Double indirect: %u\n", inode->double_indirect);
    printf("Triple indirect: %u\n", inode->triple_indirect);
}

/**
 * Print bitmap information for debugging
 *
 * @param bitmap Bitmap array to print
 */
void print_bitmap(uint8_t *bitmap)
{
    if (bitmap == NULL)
        return;

    printf("Bitmap Information:\n");
    printf("First 16 bytes: ");
    for (int i = 0; i < 16 && i < BITMAP_BYTES; i++)
    {
        printf("0x%02X ", bitmap[i]);
    }
    printf("\n");
}

int main()
{
    uint32_t next = 8;
    uint8_t bitmap[BITMAP_BYTES];
    memset(bitmap, 0, BITMAP_BYTES); // Initialize bitmap to all zeros

    // Initialize a new inode
    inode_t inode;
    init_inode(&inode, FILE_TYPE_REGULAR, 1024);

    // Write the inode to file
    const char *filename = "inodeseg0";
    if (write_inode(filename, next, bitmap, &inode, 0) != 0)
    {
        printf("Failed to write inode to file\n");
        return -1;
    }
    printf("Inode and bitmap saved to file successfully.\n");

    // Read the inode back from file
    uint32_t read_next;
    uint8_t read_bitmap[BITMAP_BYTES];
    inode_t read_inode;

    if (read_inode(filename, &read_next, read_bitmap, &read_inode, 0) != 0)
    {
        printf("Failed to read inode from file\n");
        return -1;
    }
    printf("Inode and bitmap read from file successfully.\n");

    // Print the information
    printf("\nNext inode index: %u\n", read_next);
    print_bitmap(read_bitmap);
    print_inode(&read_inode);

    return 0;
}