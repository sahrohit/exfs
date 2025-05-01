#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>

// Colors
#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_RESET "\x1b[0m"

#define SEGMENT_SIZE (1024 * 1024) // 1MB segments
#define BLOCK_SIZE 4096            // 4KB blocks
#define INODE_SIZE BLOCK_SIZE      // Each inode is one block
#define DATA_SIZE BLOCK_SIZE       // Each inode is one block

#define MAX_DIRECT_BLOCKS ((INODE_SIZE - 160) / sizeof(uint32_t)) // Rough estimate, adjust based on attributes

// Define inode file index structure and bitmap
#define MAX_INODES (SEGMENT_SIZE / INODE_SIZE)     // Maximum inodes in 1MB
#define MAX_DATA_BLOCKS (SEGMENT_SIZE / DATA_SIZE) // Maximum data blocks in 1MB

#define BITMAP_BYTES (MAX_INODES - 1) // Size of bitmap in bytes

/* File types */
#define FILE_TYPE_REGULAR 1
#define FILE_TYPE_DIRECTORY 2
#define FILE_TYPE_DATA_L1 3
#define FILE_TYPE_DATA_L2 4

/* Segment file name pattern */
#define INODE_SEGMENT_NAME_PATTERN "inodeseg%d"
#define DATA_SEGMENT_NAME_PATTERN "dataseg%d"

// Defining placeholder value
#define MAX_UNIT_32 (UINT32_MAX - 1)
#define MAX_UNIT_64 (UINT64_MAX - 1)

#define USE_SINGLE_INDIRECT 0

typedef struct
{
    uint32_t type;                             // File type (regular or directory)
    uint64_t size;                             // File size in bytes
    uint32_t direct_blocks[MAX_DIRECT_BLOCKS]; // Direct block pointers
    uint32_t single_indirect;                  // Single indirect block
    uint32_t double_indirect;                  // Double indirect block
    // uint32_t triple_indirect;                  // Triple indirect block
} inode_t;

typedef struct
{
    char data[BLOCK_SIZE]; // Data block content
} datablock_t;

// Create a new struct similar to datablock_t that stores the name and inode number of the file
typedef struct
{
    char name[20];         // File name
    uint32_t inode_number; // Inode number
    uint32_t type;         // File type (regular or directory)
    uint32_t inuse;        // In-use flag
} directory_entry_t;

#define MAX_DIRECTORY_ENTRIES (BLOCK_SIZE / sizeof(directory_entry_t)) // Maximum entries in a directory block

typedef struct
{
    directory_entry_t entries[MAX_DIRECTORY_ENTRIES]; // Directory entries
} directoryblock_t;

// Create a function read_directory_block that takes a directory block number and read the directory block from the segment file. If the directory block number is greater than 255 take divisor as a file name number and take the remainder as the directory block number. Read the segment file and read the directory block from the file. If the file is not found return -1. If the directory block is not found return -2. If the directory block is found return 0.
int read_directory_block(int directory_block_number, directoryblock_t *directory_block)
{
    uint8_t bitmap[MAX_DATA_BLOCKS];
    FILE *file = NULL;
    char filename[32];
    int segment_num = directory_block_number / 255;           // Calculate segment number
    int directory_block_index = directory_block_number % 255; // Calculate directory block index

    // Generate segment filename
    sprintf(filename, DATA_SEGMENT_NAME_PATTERN, segment_num);

    // Try to open the file
    file = fopen(filename, "r+b");
    if (file == NULL)
    {
        perror("Failed to open data segment file");
        return -1; // File not found
    }

    // Read the bitmap from the file
    if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
    {
        fclose(file);
        return -2; // Failed to read bitmap
    }

    // Check if the directory block is used
    if (bitmap[directory_block_index] == 0)
    {
        fclose(file);
        return -2; // Directory block not found
    }

    // Read the directory block from the file
    fseek(file, (directory_block_index + 1) * BLOCK_SIZE, SEEK_SET);
    size_t read = fread(directory_block, sizeof(directoryblock_t), 1, file);
    fclose(file);

    if (read != 1)
    {
        return -2; // Failed to read directory block
    }

    return 0; // Success
}

// Create an function to read the inode from a segment file. If the inode number is greater than 255 take divisor as a file name number and take the remainder as the inode number. Read the segment file and read the inode from the file. If the file is not found return -1. If the inode is not found return -2. If the inode is found return 0.
int read_inode(int inode_number, inode_t *inode)
{
    uint8_t bitmap[BITMAP_BYTES];
    FILE *file = NULL;
    char filename[32];
    int segment_num = inode_number / 255; // Calculate segment number
    int inode_index = inode_number % 255; // Calculate inode index

    // Generate segment filename
    sprintf(filename, INODE_SEGMENT_NAME_PATTERN, segment_num);

    // Try to open the file
    file = fopen(filename, "r+b");
    if (file == NULL)
    {
        perror("Failed to open inode segment file");
        return -1; // File not found
    }

    // Read the bitmap from the file
    if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
    {
        fclose(file);
        return -2; // Failed to read bitmap
    }

    // Check if the inode is used
    if (bitmap[inode_index] == 0)
    {
        fclose(file);
        return -2; // Inode not found
    }

    // Read the inode from the file
    fseek(file, (inode_index + 1) * INODE_SIZE, SEEK_SET);
    size_t read = fread(inode, sizeof(inode_t), 1, file);
    fclose(file);

    if (read != 1)
    {
        return -2; // Failed to read inode
    }

    return 0; // Success
}

// Create a read data block function that takes a datablock number and read the datablock from the segment file. If the datablock number is greater than 255 take divisor as a file name number and take the remainder as the datablock number. Read the segment file and read the datablock from the file. If the file is not found return -1. If the datablock is not found return -2. If the datablock is found return 0.
int read_datablock(int datablock_number, datablock_t *datablock)
{
    uint8_t bitmap[BITMAP_BYTES];
    FILE *file = NULL;
    char filename[32];
    int segment_num = datablock_number / 255;     // Calculate segment number
    int datablock_index = datablock_number % 255; // Calculate datablock index

    // Generate segment filename
    sprintf(filename, DATA_SEGMENT_NAME_PATTERN, segment_num);

    // Try to open the file
    file = fopen(filename, "r+b");
    if (file == NULL)
    {
        perror("Failed to open data segment file");
        return -1; // File not found
    }

    // Read the bitmap from the file
    if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
    {
        fclose(file);
        return -2; // Failed to read bitmap
    }

    // Check if the datablock is used
    if (bitmap[datablock_index] == 0)
    {
        fclose(file);
        return -2; // Datablock not found
    }

    // Read the datablock from the file
    fseek(file, (datablock_index + 1) * DATA_SIZE, SEEK_SET);
    size_t read = fread(datablock, sizeof(datablock_t), 1, file);
    fclose(file);

    if (read != 1)
    {
        return -2; // Failed to read datablock
    }

    return 0; // Success
}

// Create an inode and save it to the first available free block in an available segment
int create_inode(inode_t *inode)
{
    uint8_t bitmap[BITMAP_BYTES];
    FILE *file = NULL;
    char filename[32];
    int segment_num = 0;
    int found_space = 0;

    // Try segments until we find one with free space
    while (!found_space)
    {
        // Generate segment filename
        sprintf(filename, INODE_SEGMENT_NAME_PATTERN, segment_num);

        // printf("Checking segment file %s...\n", filename);

        // Try to open the file
        file = fopen(filename, "r+b");
        if (file == NULL)
        {
            // File doesn't exist, create a new segment
            file = fopen(filename, "w+b");
            if (file == NULL)
            {
                perror("Failed to create inode segment file");
                return -1;
            }

            // Initialize new bitmap for this segment
            memset(&bitmap, 0, BITMAP_BYTES);

            fwrite(&bitmap, sizeof(bitmap), 1, file);
            found_space = 1;
        }
        else
        {
            // Read the bitmap from existing segment
            if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
            {
                fclose(file);
                segment_num++;
                continue;
            }

            // Check if there's free space
            for (int i = 0; i < BITMAP_BYTES; i++)
            {
                if (bitmap[i] == 0)
                {
                    found_space = 1;
                    break;
                }
            }

            if (!found_space)
            {
                fclose(file);
                segment_num++;
            }
        }
    }

    // Find an empty block in the bitmap
    for (int i = 0; i < BITMAP_BYTES; i++)
    {
        if (bitmap[i] == 0)
        { // Check if the block is free
            // Mark the block as used
            bitmap[i] = 1;

            // printf("Current Bitmap: ");
            // for (int j = 0; j < BITMAP_BYTES; j++)
            // {
            //     printf("%u ", bitmap[j]);
            // }
            // printf("\n");

            // Update the bitmap in the file
            fseek(file, 0, SEEK_SET);
            fwrite(&bitmap, sizeof(bitmap), 1, file);

            // Write the inode to the file
            fseek(file, (i + 1) * INODE_SIZE, SEEK_SET);
            size_t written = fwrite(inode, sizeof(inode_t), 1, file);
            if (written != 1)
            {
                perror("Failed to write inode to file");
                fclose(file);
                return -1;
            }

            fclose(file);
            return i; // Return inode index for success
        }
    }

    // Should never reach here because we already checked for free space
    fclose(file);
    return -1;
}

int create_datablock(datablock_t *datablock)
{
    uint8_t bitmap[BITMAP_BYTES];
    FILE *file = NULL;
    char filename[32];
    int segment_num = 0;
    int found_space = 0;

    // Try segments until we find one with free space
    while (!found_space)
    {
        // Generate segment filename
        sprintf(filename, DATA_SEGMENT_NAME_PATTERN, segment_num);

        // printf("Checking segment file %s...\n", filename);

        // Try to open the file
        file = fopen(filename, "r+b");
        if (file == NULL)
        {
            // File doesn't exist, create a new segment
            file = fopen(filename, "w+b");
            if (file == NULL)
            {
                perror("Failed to create inode segment file");
                return -1;
            }

            // Initialize new bitmap for this segment
            memset(&bitmap, 0, BITMAP_BYTES);

            fwrite(&bitmap, sizeof(bitmap), 1, file);
            found_space = 1;
        }
        else
        {
            // Read the bitmap from existing segment
            if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
            {
                fclose(file);
                segment_num++;
                continue;
            }

            // Check if there's free space
            for (int i = 0; i < BITMAP_BYTES; i++)
            {
                if (bitmap[i] == 0)
                {
                    found_space = 1;
                    break;
                }
            }

            if (!found_space)
            {
                fclose(file);
                segment_num++;
            }
        }
    }

    // Find an empty block in the bitmap
    for (int i = 0; i < BITMAP_BYTES; i++)
    {
        if (bitmap[i] == 0)
        { // Check if the block is free
            // Mark the block as used
            bitmap[i] = 1;

            // printf("Current Bitmap: ");
            // for (int j = 0; j < BITMAP_BYTES; j++)
            // {
            //     printf("%u ", bitmap[j]);
            // }
            // printf("\n");

            // Update the bitmap in the file
            fseek(file, 0, SEEK_SET);
            fwrite(&bitmap, sizeof(bitmap), 1, file);

            // Write the inode to the file
            fseek(file, (i + 1) * DATA_SIZE, SEEK_SET);
            size_t written = fwrite(datablock, sizeof(datablock_t), 1, file);
            if (written != 1)
            {
                perror("Failed to write inode to file");
                fclose(file);
                return -1;
            }

            fclose(file);
            return i; // Return data index for success
        }
    }

    // Should never reach here because we already checked for free space
    fclose(file);
    return -1;
}

// Create a function create_directoryblock that takes a directoryblock and create a directoryblock in the file system. The directoryblock is created same as the create_datablock function. The difference is that instead of storing the datablock it stores a directory_block. The function returns the index of the directoryblock.
int create_directoryblock(directoryblock_t *directory_block)
{
    uint8_t bitmap[BITMAP_BYTES];
    FILE *file = NULL;
    char filename[32];
    int segment_num = 0;
    int found_space = 0;

    // Try segments until we find one with free space
    while (!found_space)
    {
        // Generate segment filename
        sprintf(filename, DATA_SEGMENT_NAME_PATTERN, segment_num);

        // printf("Checking segment file %s...\n", filename);

        // Try to open the file
        file = fopen(filename, "r+b");
        if (file == NULL)
        {
            // File doesn't exist, create a new segment
            file = fopen(filename, "w+b");
            if (file == NULL)
            {
                perror("Failed to create inode segment file");
                return -1;
            }

            // Initialize new bitmap for this segment
            memset(&bitmap, 0, BITMAP_BYTES);

            fwrite(&bitmap, sizeof(bitmap), 1, file);
            found_space = 1;
        }
        else
        {
            // Read the bitmap from existing segment
            if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
            {
                fclose(file);
                segment_num++;
                continue;
            }

            // Check if there's free space
            for (int i = 0; i < BITMAP_BYTES; i++)
            {
                if (bitmap[i] == 0)
                {
                    found_space = 1;
                    break;
                }
            }

            if (!found_space)
            {
                fclose(file);
                segment_num++;
            }
        }
    }

    // Find an empty block in the bitmap
    for (int i = 0; i < BITMAP_BYTES; i++)
    {
        if (bitmap[i] == 0)
        { // Check if the block is free
            // Mark the block as used
            bitmap[i] = 1;

            // printf("Current Bitmap: ");
            // for (int j = 0; j < BITMAP_BYTES; j++)
            // {
            //     printf("%u ", bitmap[j]);
            // }
            // printf("\n");

            // Update the bitmap in the file
            fseek(file, 0, SEEK_SET);
            fwrite(&bitmap, sizeof(bitmap), 1, file);

            // Write the inode to the file
            fseek(file, (i + 1) * DATA_SIZE, SEEK_SET);
            size_t written = fwrite(directory_block, sizeof(directoryblock_t), 1, file);
            if (written != 1)
            {
                perror("Failed to write inode to file");
                fclose(file);
                return -1;
            }
            fclose(file);
            // Returning the overall index of the directory block
            return (segment_num * 255) + i; // Return data index for success
        }
    }
    // Should never reach here because we already checked for free space
    fclose(file);
    return -1;
}

// Create a function add_directoryentry_to_directoryblock that takes a directoryblock and update its array of directory entires. The function takes in directoryblock and a directory entry and adds that directory entry to the directoryblock. The function returns 0 on success and -1 on failure.

int add_directoryentry_to_directoryblock(uint32_t directoryblock_index, directory_entry_t *entry)
{
    directoryblock_t directory_block;

    // Read the existing directory block
    int result = read_directory_block(directoryblock_index, &directory_block);
    if (result < 0)
    {
        return result; // Failed to read the directory block
    }

    // Check if there's space for a new entry
    for (int i = 0; i < BLOCK_SIZE / sizeof(directory_entry_t); i++)
    {
        if (directory_block.entries[i].inuse != 1)
        {
            // Add the new entry to the first empty slot
            directory_block.entries[i] = *entry;

            // Update the directory block in the file
            uint8_t bitmap[BITMAP_BYTES];
            FILE *file = NULL;
            char filename[32];
            int segment_num = directoryblock_index / 255;
            int block_index = directoryblock_index % 255;

            sprintf(filename, DATA_SEGMENT_NAME_PATTERN, segment_num);
            file = fopen(filename, "r+b");
            if (file == NULL)
            {
                return -1;
            }

            // Skip the bitmap
            fseek(file, (block_index + 1) * BLOCK_SIZE, SEEK_SET);

            // Write the updated directory block
            if (fwrite(&directory_block, sizeof(directoryblock_t), 1, file) != 1)
            {
                fclose(file);
                return -1;
            }

            fclose(file);

            return 0; // Success
        }
    }

    return -1; // Directory block is full
}

// Create a function create_directory that takes a directory name and create a directory in the file system. The directory is created by creating a datablock and writing the directory name to the datablock. The datablock is then saved to the first available free block in an available segment. The function returns the index of the datablock. The second paramter is the inode number of the parent directory. The function creates a directory entry in the parent directory for the new directory. If the second parameter is not provided then the parent directory is set to have inode number 0. The function returns the index of the datablock.
// It creates a directory entry in the parent directory for the new directory. If the second parameter is not provided then the parent directory is set to have inode number 0. The function returns the index of the datablock.
int create_directory(const char *directory_name, int parent_inode_number)
{
    directoryblock_t directory_block;
    uint32_t block_count = 0;

    // Create a new directory entry
    directory_entry_t new_entry;
    strncpy(new_entry.name, directory_name, sizeof(new_entry.name) - 1);
    new_entry.name[sizeof(new_entry.name) - 1] = '\0'; // Ensure null termination
    new_entry.inode_number = 0;                        // Placeholder for inode number
    new_entry.type = FILE_TYPE_DIRECTORY;              // Directory type
    new_entry.inuse = 1;                               // Mark as in-use

    // Initialize the directory block
    memset(&directory_block, 0, sizeof(directoryblock_t));
    memcpy(&directory_block.entries[0], &new_entry, sizeof(directory_entry_t));

    // Create a datablock and store its index
    int directoryblock_index = create_directoryblock(&directory_block);
    if (directoryblock_index < 0)
    {
        perror("Failed to create datablock");
        return -1;
    }

    // Save the datablock index in the parent inode's direct blocks
    if (parent_inode_number > 0)
    {
        inode_t parent_inode;
        if (read_inode(parent_inode_number, &parent_inode) != 0)
        {
            perror("Failed to read parent inode");
            return -1;
        }

        for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
        {
            if (parent_inode.direct_blocks[i] == 0)
            {
                parent_inode.direct_blocks[i] = directoryblock_index;
                break;
            }
        }

        // Update the parent inode in the segment file
        FILE *segment_file = fopen(INODE_SEGMENT_NAME_PATTERN, "r+b");
        if (segment_file == NULL)
        {
            perror("Failed to open segment file");
            return -1;
        }
        fseek(segment_file, (parent_inode_number + 1) * INODE_SIZE, SEEK_SET);
        fwrite(&parent_inode, sizeof(inode_t), 1, segment_file);
        fclose(segment_file);
    }

    return directoryblock_index; // Return the index of the created datablock
}

// Function that takes a file path and create a inode for that file and save it to the first available free block in an available segment and then create a datablock for that file and save it to the first available free block in an available segment. Save the datablock index in the inode.direct_blocks[0].
int create_inode_for_file(const char *file_path)
{
    inode_t inode;

    inode.type = FILE_TYPE_REGULAR; // Regular file
    inode.single_indirect = MAX_UNIT_32;
    inode.double_indirect = MAX_UNIT_32;

    datablock_t datablock;
    uint32_t block_count = 0;

    // Reading the actual file data
    FILE *file = fopen(file_path, "rb");
    if (file == NULL)
    {
        perror("Failed to open file");
        return -1;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    inode.size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // printf("File Size: %lu\n", inode.size);
    // printf("MAX_DIRECT_BLOCKS: %d\n", MAX_DIRECT_BLOCKS);

    // Calculate how many blocks we need
    block_count = (inode.size + BLOCK_SIZE - 1) / BLOCK_SIZE; // Ceiling division

    // File too large for direct blocks or single indirect blocks
    if ((USE_SINGLE_INDIRECT && block_count > MAX_DIRECTORY_ENTRIES) || (!USE_SINGLE_INDIRECT && block_count > MAX_DIRECT_BLOCKS))
    {
        fprintf(stderr, "File too large for direct blocks or single indirect blocks\n");
        fclose(file);
        return -1;
    }
    else
    {
        if (USE_SINGLE_INDIRECT)
        {
            // Handle large files with single indirect blocks
            inode.type = FILE_TYPE_REGULAR;

            // Allocate a directory block for indirect references
            directoryblock_t indirect_block;
            memset(&indirect_block, 0, sizeof(directoryblock_t));

            // Set up all the directory entries to be empty initially
            for (int i = 0; i < MAX_DIRECTORY_ENTRIES; i++)
            {
                indirect_block.entries[i].inuse = 0;
                indirect_block.entries[i].inode_number = MAX_UNIT_32;
                indirect_block.entries[i].type = FILE_TYPE_DATA_L1;
                strncpy(indirect_block.entries[i].name, "", sizeof(indirect_block.entries[i].name) - 1);
                indirect_block.entries[i].name[sizeof(indirect_block.entries[i].name) - 1] = '\0'; // Ensure null termination
            }

            // Create the indirect block and store its index
            int indirect_block_index = create_directoryblock(&indirect_block);
            if (indirect_block_index < 0)
            {
                fprintf(stderr, "Failed to create indirect block\n");
                fclose(file);
                return -1;
            }

            // Store the indirect block index
            inode.single_indirect = indirect_block_index;

            // Read file data in chunks and create datablocks
            int chunks_created = 0;
            for (uint32_t i = 0; i < block_count; i++)
            {

                // Skip direct blocks - we'll use indirect blocks for all chunks
                if (chunks_created >= 128)
                {
                    fprintf(stderr, "File too large even for single indirect blocks\n");
                    fclose(file);
                    return -1;
                }

                // Clear the datablock
                memset(&datablock, 0, sizeof(datablock_t));

                // Read up to BLOCK_SIZE bytes into the datablock
                size_t bytes_read = fread(datablock.data, 1, BLOCK_SIZE, file);

                // Create a datablock and store its index
                int datablock_index = create_datablock(&datablock);
                if (datablock_index < 0)
                {
                    fprintf(stderr, "Failed to create datablock\n");
                    fclose(file);
                    return -1;
                }

                // Create directory entry for this chunk
                directory_entry_t chunk_entry;
                chunk_entry.inuse = 1;
                chunk_entry.type = FILE_TYPE_DATA_L1;
                chunk_entry.inode_number = datablock_index;
                sprintf(chunk_entry.name, "chunk%d", chunks_created);

                // Print the indirect_block_index and chunk entry
                // printf("Indirect Block Index: %d, Chunk Entry: %d\n", indirect_block_index, chunk_entry.inode_number);

                // Add the entry to our indirect block
                if (add_directoryentry_to_directoryblock(indirect_block_index, &chunk_entry) < 0)
                {
                    fprintf(stderr, "Failed to add entry to indirect block\n");
                    fclose(file);
                    return -1;
                }

                chunks_created++;
            }
        }
        else
        {
            // Original code for small files (using direct blocks)
            // prefill inode MAX_DIRECT_BLOCKS with 0
            for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
            {
                inode.direct_blocks[i] = MAX_UNIT_32;
            }

            // Read file data in chunks and create datablocks
            for (uint32_t i = 0; i < block_count; i++)
            {
                // Clear the datablock
                memset(&datablock, 0, sizeof(datablock_t));

                // Read up to BLOCK_SIZE bytes into the datablock
                size_t bytes_read = fread(datablock.data, 1, BLOCK_SIZE, file);

                // Create a datablock and store its index
                int datablock_index = create_datablock(&datablock);
                if (datablock_index < 0)
                {
                    perror("Failed to create datablock");
                    fclose(file);
                    return -1;
                }

                // Save datablock index in inode
                inode.direct_blocks[i] = datablock_index;
            }
        }
    }

    // // prefill inode MAX_DIRECT_BLOCKS with 0
    // for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
    // {
    //     inode.direct_blocks[i] = MAX_UNIT_32;
    // }

    // // Read file data in chunks and create datablocks
    // for (uint32_t i = 0; i < block_count; i++)
    // {
    //     // Clear the datablock
    //     memset(&datablock, 0, sizeof(datablock_t));

    //     // Read up to BLOCK_SIZE bytes into the datablock
    //     size_t bytes_read = fread(datablock.data, 1, BLOCK_SIZE, file);

    //     // Create a datablock and store its index
    //     int datablock_index = create_datablock(&datablock);
    //     if (datablock_index < 0)
    //     {
    //         perror("Failed to create datablock");
    //         fclose(file);
    //         return -1;
    //     }

    //     // Save datablock index in inode
    //     inode.direct_blocks[i] = datablock_index;
    // }

    // printf("Total Block Count %d\n", block_count);

    // Print the inode.direct_blocks that was going to be written to the file
    // printf("Inode Direct Blocks: ");
    // for (int i = 0; i < block_count; i++)
    // {
    //     printf("%u ", inode.direct_blocks[i]);
    // }
    // printf("\n");

    fclose(file);

    //  inode.triple_indirect = 0;
    // for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
    // {
    //     inode.direct_blocks[i] = 0; // Initialize direct blocks
    // }
    // Create inode and datablock

    int inode_index = create_inode(&inode);
    if (inode_index < 0)
    {
        perror("Failed to create inode");
        return -1;
    }

    // // Update inode in the segment file
    // FILE *segment_file = fopen(INODE_SEGMENT_NAME_PATTERN, "r+b");
    // if (segment_file == NULL)
    // {
    //     perror("Failed to open segment file");
    //     return -1;
    // }
    // fseek(segment_file, (inode_index + 1) * INODE_SIZE, SEEK_SET);
    // fwrite(&inode, sizeof(inode_t), 1, segment_file);
    // fclose(segment_file);
    return inode_index;
}

int find_entry_in_directory(directoryblock_t *dir_block, const char *name, directory_entry_t **found_entry)
{
    for (int i = 0; i < MAX_DIRECTORY_ENTRIES; i++)
    {
        if (dir_block->entries[i].inuse == 1 && strcmp(dir_block->entries[i].name, name) == 0)
        {
            *found_entry = &dir_block->entries[i];
            return i; // Return index if found
        }
    }
    *found_entry = NULL;
    return -1; // Not found
}

// Split a path into segments and return the count of segments
int split_path(const char *path, char *segments[], int max_segments)
{
    char path_copy[256]; // Create a copy since strtok modifies the string
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0'; // Ensure null termination

    int segment_count = 0;
    char *segment = strtok(path_copy, "/");

    while (segment != NULL && segment_count < max_segments)
    {
        segments[segment_count++] = strdup(segment);
        segment = strtok(NULL, "/");
    }
    segments[segment_count] = NULL;

    return segment_count;
}

// Create a function to add file to the filesystem
int add_file(const char *fs_path, const char *local_file)
{

    // In the add_file function:
    char *path_segments[256];
    int segment_count = split_path(fs_path, path_segments, 256);

    // // Print the segments
    // printf("Path Segments:\n");
    // for (int i = 0; i < segment_count; i++)
    // {
    //     printf("%s\n", path_segments[i]);
    // }

    // printf("Size of file %d", sizeof(local_file));

    size_t inode_index = create_inode_for_file(local_file);
    if (inode_index < 0)
    {
        fprintf(stderr, "Failed to create inode for file\n");
        return -1;
    }

    directoryblock_t directoryblock;

    for (int i = 0; i < MAX_DIRECTORY_ENTRIES; i++)
    {
        directoryblock.entries[i].inode_number = MAX_UNIT_32;
        directoryblock.entries[i].type = MAX_UNIT_32;
        directoryblock.entries[i].inuse = 0;
        strncpy(directoryblock.entries[i].name, "", sizeof(directoryblock.entries[i].name) - 1);
        directoryblock.entries[i].name[sizeof(directoryblock.entries[i].name) - 1] = '\0'; // Ensure null termination
    }

    int root_directoryblock_index = create_directoryblock(&directoryblock);

    // Adding the current file to the root directory
    directory_entry_t new_entry;
    new_entry.inode_number = inode_index; // Placeholder for inode number
    new_entry.type = FILE_TYPE_REGULAR;
    strncpy(new_entry.name, path_segments[segment_count - 1], sizeof(new_entry.name) - 1);
    new_entry.name[sizeof(new_entry.name) - 1] = '\0';                           // Ensure null termination
                                                                                 // Directory type
    new_entry.inuse = 1;                                                         // Mark as in-use
    add_directoryentry_to_directoryblock(root_directoryblock_index, &new_entry); // Adding the file to the root directory

    // Create directory for each segment if the directory is already not present and link them together with inodes in between them.
    //     If the fs_path is /dir1/dir2/sample.txt
    // Then the root inode at inode_index 0 will have direct_blocks mapping to directory_block at index 0.
    // Now inode for dir1 is created and then a new directory entry for under the root_directory block is created storing name as dir1 and the inode_number for recently created inode of dir1.
    // Similarly, do the same for dir2 and save its inode_number in dir1 directory block that is accessed in inode direct_blocks[0].
    // Now create a inode of type file and point it to the file created and then save the inode number and file name in the directory block of dir2.
    // So now the chain would look something like this root inode -> root directory block -> dir1 inode -> dir1 directory block -> dir2 inode -> dir2 directory block -> sample.txt inode -> sample.txt directory block.
    // For accessing each directory block, the inode of each has direct_blocks array where the first element of that direct_blocks array has index in the directory/datablock segment.

    // Start with root inode (assumed to be at index 0)
    int current_inode_index = 0;
    inode_t current_inode;
    directoryblock_t current_dir_block;

    // For each directory segment (except the last one which is the file)
    for (int i = 0; i < segment_count - 1; i++)
    {
        // Read the current inode
        if (read_inode(current_inode_index, &current_inode) < 0)
        {
            fprintf(stderr, "Failed to read inode at index %d\n", current_inode_index);
            return -1;
        }

        // Find the directory block for this inode
        int dir_block_index = -1;
        for (int j = 0; j < MAX_DIRECT_BLOCKS; j++)
        {
            if (current_inode.direct_blocks[j] != MAX_UNIT_32 &&
                current_inode.direct_blocks[j] != 0)
            {
                dir_block_index = current_inode.direct_blocks[j];
                break;
            }
        }

        if (dir_block_index < 0)
        {
            // This inode doesn't point to any directory block yet, create one
            for (int k = 0; k < MAX_DIRECTORY_ENTRIES; k++)
            {
                directoryblock.entries[k].inuse = 0;
            }
            dir_block_index = create_directoryblock(&directoryblock);
            if (dir_block_index < 0)
            {
                fprintf(stderr, "Failed to create directory block\n");
                return -1;
            }

            // Update the inode to point to this directory block
            for (int j = 0; j < MAX_DIRECT_BLOCKS; j++)
            {
                if (current_inode.direct_blocks[j] == 0 ||
                    current_inode.direct_blocks[j] == MAX_UNIT_32)
                {
                    current_inode.direct_blocks[j] = dir_block_index;

                    // Write the updated inode
                    FILE *inode_file = NULL;
                    char inode_filename[32];
                    int segment_num = current_inode_index / 255;
                    int inode_idx = current_inode_index % 255;

                    sprintf(inode_filename, INODE_SEGMENT_NAME_PATTERN, segment_num);
                    inode_file = fopen(inode_filename, "r+b");
                    if (inode_file == NULL)
                    {
                        return -1;
                    }

                    fseek(inode_file, (inode_idx + 1) * INODE_SIZE, SEEK_SET);
                    fwrite(&current_inode, sizeof(inode_t), 1, inode_file);
                    fclose(inode_file);
                    break;
                }
            }
        }

        // Read the directory block
        if (read_directory_block(dir_block_index, &current_dir_block) < 0)
        {
            fprintf(stderr, "Failed to read directory block at index %d\n", dir_block_index);
            return -1;
        }

        // Look for the directory entry for the next path segment
        directory_entry_t *found_entry = NULL;
        int entry_index = find_entry_in_directory(&current_dir_block, path_segments[i], &found_entry);

        if (entry_index < 0)
        {
            // Directory doesn't exist, create it
            inode_t new_dir_inode;
            memset(&new_dir_inode, 0, sizeof(inode_t));
            new_dir_inode.type = FILE_TYPE_DIRECTORY;
            new_dir_inode.single_indirect = MAX_UNIT_32;
            new_dir_inode.double_indirect = MAX_UNIT_32;
            for (int j = 0; j < MAX_DIRECT_BLOCKS; j++)
            {
                new_dir_inode.direct_blocks[j] = MAX_UNIT_32;
            }

            // Create the new inode for the directory
            int new_inode_index = create_inode(&new_dir_inode);
            if (new_inode_index < 0)
            {
                fprintf(stderr, "Failed to create inode for directory %s\n", path_segments[i]);
                return -1;
            }

            // Add directory entry to the current directory block
            directory_entry_t new_entry;
            new_entry.inode_number = new_inode_index;
            new_entry.type = FILE_TYPE_DIRECTORY;
            new_entry.inuse = 1;
            strncpy(new_entry.name, path_segments[i], sizeof(new_entry.name) - 1);
            new_entry.name[sizeof(new_entry.name) - 1] = '\0';

            if (add_directoryentry_to_directoryblock(dir_block_index, &new_entry) < 0)
            {
                fprintf(stderr, "Failed to add directory entry for %s\n", path_segments[i]);
                return -1;
            }

            current_inode_index = new_inode_index;
        }
        else
        {
            // Directory exists, continue with its inode
            current_inode_index = found_entry->inode_number;
        }
    }

    // Now add the file to the last directory
    if (read_inode(current_inode_index, &current_inode) < 0)
    {
        fprintf(stderr, "Failed to read final directory inode\n");
        return -1;
    }

    // Find the directory block for this inode
    int dir_block_index = -1;
    for (int j = 0; j < MAX_DIRECT_BLOCKS; j++)
    {
        if (current_inode.direct_blocks[j] != MAX_UNIT_32 &&
            current_inode.direct_blocks[j] != 0)
        {
            dir_block_index = current_inode.direct_blocks[j];
            break;
        }
    }

    if (dir_block_index < 0)
    {
        // Create a new directory block
        for (int k = 0; k < MAX_DIRECTORY_ENTRIES; k++)
        {
            directoryblock.entries[k].inuse = 0;
        }
        dir_block_index = create_directoryblock(&directoryblock);
        if (dir_block_index < 0)
        {
            fprintf(stderr, "Failed to create directory block for file\n");
            return -1;
        }

        // Update the inode to point to this directory block
        for (int j = 0; j < MAX_DIRECT_BLOCKS; j++)
        {
            if (current_inode.direct_blocks[j] == 0 ||
                current_inode.direct_blocks[j] == MAX_UNIT_32)
            {
                current_inode.direct_blocks[j] = dir_block_index;

                // Write the updated inode
                FILE *inode_file = NULL;
                char inode_filename[32];
                int segment_num = current_inode_index / 255;
                int inode_idx = current_inode_index % 255;

                sprintf(inode_filename, INODE_SEGMENT_NAME_PATTERN, segment_num);
                inode_file = fopen(inode_filename, "r+b");
                if (inode_file == NULL)
                {
                    return -1;
                }

                fseek(inode_file, (inode_idx + 1) * INODE_SIZE, SEEK_SET);
                fwrite(&current_inode, sizeof(inode_t), 1, inode_file);
                fclose(inode_file);
                break;
            }
        }
    }

    // Add file entry to final directory
    directory_entry_t file_entry;
    file_entry.inode_number = inode_index;
    file_entry.type = FILE_TYPE_REGULAR;
    file_entry.inuse = 1;
    strncpy(file_entry.name, path_segments[segment_count - 1], sizeof(file_entry.name) - 1);
    file_entry.name[sizeof(file_entry.name) - 1] = '\0';

    if (add_directoryentry_to_directoryblock(dir_block_index, &file_entry) < 0)
    {
        fprintf(stderr, "Failed to add file entry to directory\n");
        return -1;
    }

    // // Free allocated memory for path segments
    // for (int i = 0; i < segment_count; i++)
    // {
    //     free(path_segments[i]);
    // }

    return 0;
}

// Create a function to extract a file from the file system. The function takes a path as input and extracts the file from the file system. The function returns 0 on success and -1 on failure.
int extract_file(const char *path)
{

    char *path_segments[256];
    int segment_count = split_path(path, path_segments, 256);

    // printf("Path Segments:\n");
    // for (int i = 0; i < segment_count; i++)
    // {
    //     printf("%s\n", path_segments[i]);
    // }

    // Start with root inode (inode 0)
    int current_inode_index = 0;
    inode_t current_inode;
    directoryblock_t dir_block;

    // Traverse the path segments except the last one (which is the file)
    for (int i = 0; i < segment_count; i++)
    {
        // Read the current inode
        if (read_inode(current_inode_index, &current_inode) < 0)
        {
            fprintf(stderr, "Failed to read inode at index %d\n", current_inode_index);
            return -1;
        }

        // Make sure it's a directory (except for the last segment which could be a file)
        if (i < segment_count - 1 && current_inode.type != FILE_TYPE_DIRECTORY)
        {
            fprintf(stderr, "Path component %s is not a directory\n", path_segments[i]);
            return -1;
        }

        // For directories, look through direct blocks for the directory block
        int found_next_segment = 0;
        for (int j = 0; j < MAX_DIRECT_BLOCKS; j++)
        {
            // TODO: Remove the ==0 check as all inode will be prefilled with MAX_UNIT_32
            if (current_inode.direct_blocks[j] == MAX_UNIT_32 ||
                current_inode.direct_blocks[j] == 0)
            {
                continue;
            }

            // Read directory block
            if (read_directory_block(current_inode.direct_blocks[j], &dir_block) < 0)
            {
                fprintf(stderr, "Failed to read directory block %u\n", current_inode.direct_blocks[j]);
                continue;
            }

            // Search for the next path segment in this directory block
            for (int k = 0; k < MAX_DIRECTORY_ENTRIES; k++)
            {
                if (dir_block.entries[k].inuse == 1 &&
                    strcmp(dir_block.entries[k].name, path_segments[i]) == 0)
                {
                    // Found the entry for the next path segment
                    current_inode_index = dir_block.entries[k].inode_number;
                    found_next_segment = 1;
                    break;
                }
            }

            if (found_next_segment)
            {
                break;
            }
        }

        if (!found_next_segment)
        {
            fprintf(stderr, "Path component %s not found\n", path_segments[i]);
            return -1;
        }
    }

    inode_t file_inode;

    if (read_inode(current_inode_index, &file_inode) < 0)
    {
        fprintf(stderr, "Failed to read file inode at index %d\n", current_inode_index);
        return -1;
    }

    // If the inode doesn't have a single indirect block
    if (file_inode.single_indirect < 0 || file_inode.single_indirect == MAX_UNIT_32)
    {

        for (int m = 0; m < MAX_DIRECT_BLOCKS; m++)
        {

            if (file_inode.direct_blocks[m] == MAX_UNIT_32)
            {
                break; // No more direct blocks
            }

            datablock_t datablock;

            int result = read_datablock(file_inode.direct_blocks[m], &datablock);
            if (result < 0)
            {
                fprintf(stderr, "Failed to read datablock\n");
                return -1;
            }

            // ******************** Print the datablock details ********************

            int last_block_size = file_inode.size % BLOCK_SIZE;
            int last_block_index = file_inode.size / BLOCK_SIZE;
            int loop_till = BLOCK_SIZE;

            if (last_block_index == m)
            {
                loop_till = last_block_size;
            }

            // Print everything as removeing null characters break binary files
            for (int n = 0; n < loop_till; n++)
            {
                printf("%c", datablock.data[n]);
            }

            // ******************** Print the datablock details ********************
        }
    }
    // If the inode block has a single indirect block
    else
    {
        // Print the directory_entries of the single indirect block
        directoryblock_t indirect_block;
        if (read_directory_block(file_inode.single_indirect, &indirect_block) < 0)
        {
            fprintf(stderr, "Failed to read indirect block\n");
            return -1;
        }

        // // Print the indirect block entries details
        // for (int m = 0; m < MAX_DIRECTORY_ENTRIES; m++)
        // {
        //     if (indirect_block.entries[m].inuse == 1)
        //     {
        //         printf("Indirect Block Entry %d: Inode Number: %u, Type: %u\n",
        //                m, indirect_block.entries[m].inode_number, indirect_block.entries[m].type);
        //     }
        // }

        // Print the entries in the indirect block
        for (int m = 0; m < MAX_DIRECTORY_ENTRIES; m++)
        {
            if (indirect_block.entries[m].inuse == 1)
            {
                datablock_t datablock;
                int result = read_datablock(indirect_block.entries[m].inode_number, &datablock);
                if (result < 0)
                {
                    fprintf(stderr, "Failed to read indirect datablock\n");
                    return -1;
                }

                // ******************** Print the datablock details ********************

                int last_block_size = file_inode.size % BLOCK_SIZE;
                int last_block_index = file_inode.size / BLOCK_SIZE;
                int loop_till = BLOCK_SIZE;

                if (last_block_index == m)
                {
                    loop_till = last_block_size;
                }

                // Print everything as removeing null characters break binary files
                for (int n = 0; n < loop_till; n++)
                {
                    printf("%c", datablock.data[n]);
                }

                // ******************** Print the datablock details ********************
            }
        }
    }

    // fprintf(stderr, "Failed to extract file (path may not lead to a regular file)\n");
    return 0;
}

// Create a function to debug the path. This function takes prints the bitmap of all the segments and inodes files. Both INODE_SEGMENT_NAME_PATTERN and DATA_SEGMENT_NAME_PATTERN inital bitmap are printed here.
int debug_path(const char *path)
{
    // In the add_file function:
    char *path_segments[10];
    int segment_count = split_path(path, path_segments, 10);

    // Print the segments
    printf("Path Segments:\n");
    for (int i = 0; i < segment_count; i++)
    {
        printf("%s\n", path_segments[i]);
    }

    // Print the bitmap of all the segments and inodes files
    int segment_num = 0;
    char filename[32];
    FILE *file = NULL;
    uint8_t bitmap[BITMAP_BYTES];

    // Check inode segments
    while (1)
    {
        // Generate segment filename
        sprintf(filename, INODE_SEGMENT_NAME_PATTERN, segment_num);

        // Try to open the file
        file = fopen(filename, "rb");
        if (file == NULL)
        {
            // No more inode segments
            break;
        }

        // Read the bitmap from the file
        if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
        {
            perror("Failed to read bitmap");
            fclose(file);
            return -2;
        }

        printf("Bitmap of %s: ", filename);
        for (int j = 0; j < BITMAP_BYTES; j++)
        {
            printf("%u ", bitmap[j]);
        }
        printf("\n");

        // From the bitmap, list all the inode details that are in use
        for (int i = 0; i < BITMAP_BYTES; i++)
        {
            if (bitmap[i] == 1)
            {
                inode_t inode;
                int result = read_inode(i, &inode);
                if (result < 0)
                {
                    fprintf(stderr, "Failed to read inode\n");
                    fclose(file);
                    return -2;
                }
                printf("Inode %d: Type: %u, Size: %lu, Single Indirect %d \n", i, inode.type, inode.size, inode.single_indirect);
                // printf("Direct blocks: ");
                // for (int k = 0; k < MAX_DIRECT_BLOCKS; k++)
                // {
                //     printf("%u ", inode.direct_blocks[k]);
                // }
                // printf("\n");
            }
        }

        printf("\n");

        fclose(file);
        segment_num++;
    }

    // Reset segment_num for data segments
    segment_num = 0;

    // Check data segments
    while (1)
    {
        // Generate segment filename
        sprintf(filename, DATA_SEGMENT_NAME_PATTERN, segment_num);

        // Try to open the file
        file = fopen(filename, "rb");
        if (file == NULL)
        {
            // No more data segments
            break;
        }

        // Read the bitmap from the file
        if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
        {
            perror("Failed to read bitmap");
            fclose(file);
            return -2;
        }

        printf("Bitmap of %s: ", filename);
        for (int j = 0; j < BITMAP_BYTES; j++)
        {
            printf("%u ", bitmap[j]);
        }
        printf("\n");

        // From the bitmap, list all the datablock details that are in use
        for (int i = 0; i < BITMAP_BYTES; i++)
        {
            if (bitmap[i] == 1)
            {
                // First read it as a datablock to check the content
                datablock_t datablock;
                int result = read_datablock(i, &datablock);

                if (result < 0)
                {
                    fprintf(stderr, "Failed to read datablock %d\n", i);
                    fclose(file);
                    return -2;
                }

                // Try to read it as a directory block to check if it has valid entries
                directoryblock_t directory_block;
                if (read_directory_block(i, &directory_block) == 0)
                {
                    if (directory_block.entries[0].inuse == 1)
                    {
                        printf("Datablock %d: Directory Block, First entry: %s (inode: %u)\n",
                               i, directory_block.entries[0].name, directory_block.entries[0].inode_number);
                        continue;
                    }
                }

                // Otherwise print as regular datablock
                printf("Datablock %d: Regular Block, Size: %lu \n", i, sizeof(datablock.data));
            }
        }
        printf("\n");

        fclose(file);
        segment_num++;
    }

    return 0;
}

// Forward declaration
static void list_directory_recursive(int inode_number, int depth);

// Print entries in a directory block
static void print_directory_entries(directoryblock_t *dir_block, int depth)
{
    for (int i = 0; i < MAX_DIRECTORY_ENTRIES; i++)
    {
        if (dir_block->entries[i].inuse == 1)
        {
            // Print indentation
            for (int j = 0; j < depth; j++)
            {
                printf("│   ");
            }

            printf("├── %s [Inode: %u, %s]\n",
                   dir_block->entries[i].name,
                   dir_block->entries[i].inode_number,
                   dir_block->entries[i].type == FILE_TYPE_DIRECTORY ? "Directory" : "File");

            // Recursively list subdirectories
            if (dir_block->entries[i].type == FILE_TYPE_DIRECTORY)
            {
                list_directory_recursive(dir_block->entries[i].inode_number, depth + 1);
            }
        }
    }
}

// Recursively list contents of a directory starting from the given inode
static void list_directory_recursive(int inode_number, int depth)
{
    inode_t inode;
    int result = read_inode(inode_number, &inode);

    if (result < 0)
    {
        fprintf(stderr, "Failed to read inode %d\n", inode_number);
        return;
    }

    if (inode.type != FILE_TYPE_DIRECTORY)
    {
        return; // Not a directory
    }

    // Process each direct block that could be a directory block
    for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
    {
        if (inode.direct_blocks[i] != MAX_UNIT_32 && inode.direct_blocks[i] != 0)
        {
            directoryblock_t dir_block;
            if (read_directory_block(inode.direct_blocks[i], &dir_block) == 0)
            {
                print_directory_entries(&dir_block, depth);
            }
        }
    }
}

// List directory starting from the root inode (inode 0)
int list_directory(int unused)
{
    printf("Root [Inode: 0, Directory]\n");
    list_directory_recursive(0, 1);
    return 0;
}

// Create a function to initialize the file system that creates the first inode and first datasegments of the file system. The first inode is the root inode and the first datasegment is the root datasegment. The root inode is a directory and the root datasegment is a directory.
int init_file_system()
{
    inode_t inode;
    directoryblock_t directoryblock;
    char inodeseg_filename[32];
    char dataseg_filename[32];

    sprintf(inodeseg_filename, INODE_SEGMENT_NAME_PATTERN, 0);
    sprintf(dataseg_filename, DATA_SEGMENT_NAME_PATTERN, 0);

    // Try to read the first inode segment and first data segment, if they exist the file system is already initialized
    FILE *inode_segment = fopen(inodeseg_filename, "r");
    FILE *data_segment = fopen(dataseg_filename, "r");

    // Maybe we can only initialize the directory block upon need rather then prefilling it
    if (data_segment == NULL && inode_segment == NULL)
    {
        // for (int i = 0; i < MAX_DIRECTORY_ENTRIES; i++)
        // {
        //     if (i == 0)
        //     {

        //         directoryblock.entries[0].inode_number = -1;
        //         directoryblock.entries[0].type = FILE_TYPE_DIRECTORY;
        //         directoryblock.entries[0].inuse = 1;
        //         strncpy(directoryblock.entries[0].name, "root", sizeof(directoryblock.entries[0].name) - 1);
        //         directoryblock.entries[0].name[sizeof(directoryblock.entries[0].name) - 1] = '\0'; // Ensure null termination
        //     }
        //     else
        //     {
        //         directoryblock.entries[i].inode_number = 0;
        //         directoryblock.entries[i].type = FILE_TYPE_REGULAR;
        //         directoryblock.entries[i].inuse = 0;
        //         strncpy(directoryblock.entries[i].name, "", sizeof(directoryblock.entries[i].name) - 1);
        //         directoryblock.entries[i].name[sizeof(directoryblock.entries[i].name) - 1] = '\0'; // Ensure null termination
        //     }
        // }

        // int root_directoryblock_index = create_directoryblock(&directoryblock);

        // printf("Directory Block Size: %lu\n", sizeof(directoryblock_t));
        // printf("Directory Entry Size: %lu\n", sizeof(directory_entry_t));
        // printf("Data Block Size: %lu\n", sizeof(datablock_t));
        // printf("MAX_DIRECTORY_ENTRIES: %lu\n", MAX_DIRECTORY_ENTRIES);
        // printf("INode Size: %lu\n", sizeof(inode_t));

        // fill MAX_DIRECT_BLOCKS with 0
        for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
        {
            inode.direct_blocks[i] = 0; // Initialize direct blocks
        }

        // inode.direct_blocks[0] = root_directoryblock_index;
        inode.type = FILE_TYPE_DIRECTORY; // Directory type
        inode.size = 0;                   // Size is initially 0
        inode.single_indirect = MAX_UNIT_32;
        inode.double_indirect = MAX_UNIT_32;

        int root_inode_index = create_inode(&inode);
        if (root_inode_index < 0)
        {
            fprintf(stderr, "Failed to create root inode\n");
            return -1;
        }

        return 0;
    }
    else
    {
        fclose(data_segment);
        return 0; // File system already initialized
    }
}

// *************** Entirely AI Generated and not tested starts ***************
// Create a function remove_file that takes a path as input and removes the file from the file system. The function returns 0 on success and -1 on failure. The function navigate through the paths and recursively deletes the last segment. If its a file, just delete the file and if its a folder delete the folder and also delete everything in the folder recursively. By deleting, if its a inode then mark it as free in the bitmap and if its a datablock then mark it as free in the bitmap. The function also updates the parent directory to remove the entry for the deleted file or folder. For the directory entry, it should mark the inuse as 0.
int remove_inode_and_blocks(int inode_number);

// Helper function to mark inode as free in bitmap
int free_inode(int inode_number)
{
    FILE *file = NULL;
    char filename[32];
    int segment_num = inode_number / 255;
    int inode_index = inode_number % 255;

    sprintf(filename, INODE_SEGMENT_NAME_PATTERN, segment_num);
    file = fopen(filename, "r+b");
    if (file == NULL)
    {
        perror("Failed to open inode segment file");
        return -1;
    }

    // Update bitmap to mark inode as free
    uint8_t bitmap[BITMAP_BYTES];
    if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
    {
        fclose(file);
        return -1;
    }

    bitmap[inode_index] = 0; // Mark as free

    fseek(file, 0, SEEK_SET);
    fwrite(bitmap, sizeof(bitmap), 1, file);
    fclose(file);

    return 0;
}

// Helper function to mark datablock as free in bitmap
int free_datablock(int datablock_number)
{
    FILE *file = NULL;
    char filename[32];
    int segment_num = datablock_number / 255;
    int block_index = datablock_number % 255;

    sprintf(filename, DATA_SEGMENT_NAME_PATTERN, segment_num);
    file = fopen(filename, "r+b");
    if (file == NULL)
    {
        perror("Failed to open data segment file");
        return -1;
    }

    // Update bitmap to mark block as free
    uint8_t bitmap[BITMAP_BYTES];
    if (fread(bitmap, sizeof(bitmap), 1, file) != 1)
    {
        fclose(file);
        return -1;
    }

    bitmap[block_index] = 0; // Mark as free

    fseek(file, 0, SEEK_SET);
    fwrite(bitmap, sizeof(bitmap), 1, file);
    fclose(file);

    return 0;
}

// Recursive function to remove an inode and all associated blocks
int remove_inode_and_blocks(int inode_number)
{
    inode_t inode;
    int result = read_inode(inode_number, &inode);
    if (result < 0)
    {
        return result;
    }

    // If it's a directory, recursively remove all entries
    if (inode.type == FILE_TYPE_DIRECTORY)
    {
        for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
        {
            if (inode.direct_blocks[i] == MAX_UNIT_32 || inode.direct_blocks[i] == 0)
            {
                continue;
            }

            directoryblock_t dir_block;
            if (read_directory_block(inode.direct_blocks[i], &dir_block) == 0)
            {
                // Remove all entries in this directory block
                for (int j = 0; j < MAX_DIRECTORY_ENTRIES; j++)
                {
                    if (dir_block.entries[j].inuse == 1)
                    {
                        remove_inode_and_blocks(dir_block.entries[j].inode_number);
                    }
                }
            }

            // Free the directory block
            free_datablock(inode.direct_blocks[i]);
        }
    }
    else
    {
        // Regular file - free all datablocks
        for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
        {
            if (inode.direct_blocks[i] != MAX_UNIT_32 && inode.direct_blocks[i] != 0)
            {
                free_datablock(inode.direct_blocks[i]);
            }
        }
    }

    // Also handle indirect blocks if they're used
    if (inode.single_indirect != 0 && inode.single_indirect != MAX_UNIT_32)
    {
        // Read the indirect block and free all referenced blocks
        datablock_t indirect_block;
        if (read_datablock(inode.single_indirect, &indirect_block) == 0)
        {
            uint32_t *block_pointers = (uint32_t *)indirect_block.data;
            for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t); i++)
            {
                if (block_pointers[i] != 0 && block_pointers[i] != MAX_UNIT_32)
                {
                    free_datablock(block_pointers[i]);
                }
            }
        }
        free_datablock(inode.single_indirect);
    }

    if (inode.double_indirect != 0 && inode.double_indirect != MAX_UNIT_32)
    {
        // Read the double indirect block
        datablock_t double_indirect_block;
        if (read_datablock(inode.double_indirect, &double_indirect_block) == 0)
        {
            uint32_t *indirect_pointers = (uint32_t *)double_indirect_block.data;
            for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t); i++)
            {
                if (indirect_pointers[i] != 0 && indirect_pointers[i] != MAX_UNIT_32)
                {
                    // Read each single indirect block
                    datablock_t indirect_block;
                    if (read_datablock(indirect_pointers[i], &indirect_block) == 0)
                    {
                        uint32_t *block_pointers = (uint32_t *)indirect_block.data;
                        for (int j = 0; j < BLOCK_SIZE / sizeof(uint32_t); j++)
                        {
                            if (block_pointers[j] != 0 && block_pointers[j] != MAX_UNIT_32)
                            {
                                free_datablock(block_pointers[j]);
                            }
                        }
                    }
                    free_datablock(indirect_pointers[i]);
                }
            }
        }
        free_datablock(inode.double_indirect);
    }

    // Finally, free the inode itself
    return free_inode(inode_number);
}

int remove_file(const char *path)
{
    char *path_segments[10];
    int segment_count = split_path(path, path_segments, 10);
    if (segment_count <= 0)
    {
        return -1; // Invalid path
    }

    // Start with root inode
    int current_inode_index = 0;
    int parent_inode_index = 0;
    int parent_dir_block_index = -1;
    int entry_index_in_parent = -1;

    inode_t current_inode;
    directoryblock_t dir_block;

    // Navigate to the parent of the target file/directory
    for (int i = 0; i < segment_count - 1; i++)
    {
        if (read_inode(current_inode_index, &current_inode) < 0)
        {
            fprintf(stderr, "Failed to read inode at index %d\n", current_inode_index);
            return -1;
        }

        if (current_inode.type != FILE_TYPE_DIRECTORY)
        {
            fprintf(stderr, "Path component %s is not a directory\n", path_segments[i]);
            return -1;
        }

        int found_next_segment = 0;
        for (int j = 0; j < MAX_DIRECT_BLOCKS; j++)
        {
            if (current_inode.direct_blocks[j] == MAX_UNIT_32 ||
                current_inode.direct_blocks[j] == 0)
            {
                continue;
            }

            if (read_directory_block(current_inode.direct_blocks[j], &dir_block) < 0)
            {
                continue;
            }

            for (int k = 0; k < MAX_DIRECTORY_ENTRIES; k++)
            {
                if (dir_block.entries[k].inuse == 1 &&
                    strcmp(dir_block.entries[k].name, path_segments[i]) == 0)
                {
                    parent_inode_index = current_inode_index;
                    current_inode_index = dir_block.entries[k].inode_number;
                    found_next_segment = 1;
                    break;
                }
            }

            if (found_next_segment)
            {
                break;
            }
        }

        if (!found_next_segment)
        {
            fprintf(stderr, "Path component %s not found\n", path_segments[i]);
            return -1;
        }
    }

    // Now current_inode_index points to the parent directory
    parent_inode_index = current_inode_index;

    // Find the target file/directory in the parent
    if (read_inode(parent_inode_index, &current_inode) < 0)
    {
        fprintf(stderr, "Failed to read parent directory inode\n");
        return -1;
    }

    int target_inode_index = -1;
    int found_target = 0;

    for (int j = 0; j < MAX_DIRECT_BLOCKS; j++)
    {
        if (current_inode.direct_blocks[j] == MAX_UNIT_32 ||
            current_inode.direct_blocks[j] == 0)
        {
            continue;
        }

        if (read_directory_block(current_inode.direct_blocks[j], &dir_block) < 0)
        {
            continue;
        }

        for (int k = 0; k < MAX_DIRECTORY_ENTRIES; k++)
        {
            if (dir_block.entries[k].inuse == 1 &&
                strcmp(dir_block.entries[k].name, path_segments[segment_count - 1]) == 0)
            {
                target_inode_index = dir_block.entries[k].inode_number;
                parent_dir_block_index = current_inode.direct_blocks[j];
                entry_index_in_parent = k;
                found_target = 1;
                break;
            }
        }

        if (found_target)
        {
            break;
        }
    }

    if (!found_target || target_inode_index < 0)
    {
        fprintf(stderr, "Target %s not found in parent directory\n", path_segments[segment_count - 1]);
        return -1;
    }

    // Remove the target file/directory recursively
    if (remove_inode_and_blocks(target_inode_index) < 0)
    {
        fprintf(stderr, "Failed to remove target inode\n");
        return -1;
    }

    // Update parent directory to remove the entry
    if (read_directory_block(parent_dir_block_index, &dir_block) < 0)
    {
        fprintf(stderr, "Failed to read parent directory block\n");
        return -1;
    }

    // Mark the directory entry as not in use
    dir_block.entries[entry_index_in_parent].inuse = 0;

    // Write the updated directory block back
    FILE *file = NULL;
    char filename[32];
    int segment_num = parent_dir_block_index / 255;
    int block_index = parent_dir_block_index % 255;

    sprintf(filename, DATA_SEGMENT_NAME_PATTERN, segment_num);
    file = fopen(filename, "r+b");
    if (file == NULL)
    {
        return -1;
    }

    fseek(file, (block_index + 1) * BLOCK_SIZE, SEEK_SET);
    fwrite(&dir_block, sizeof(directoryblock_t), 1, file);
    fclose(file);

    // // Free allocated memory for path segments
    // for (int i = 0; i < segment_count; i++)
    // {
    //     free(path_segments[i]);
    // }

    return 0;
}

// *************** Entirely AI Generated and not tested ends ***************

/*
 * Implementation
 */

int main(int argc, char *argv[])
{

    printf("MAX_DIRECT_BLOCKS: %d\n", MAX_DIRECT_BLOCKS);
    printf("MAX_DIRECTORY_ENTRIES: %d\n", MAX_DIRECTORY_ENTRIES);

    int opt;
    char *fs_path = NULL;
    char *local_file = NULL;

    // Initialize file system
    if (init_file_system() != 0)
    {
        fprintf(stderr, "Failed to initialize file system\n");
        return 1;
    }

    // Parse command line arguments
    while ((opt = getopt(argc, argv, "la:f:r:e:D:")) != -1)
    {
        switch (opt)
        {
        case 'l': // List directory
            return list_directory(0);

        case 'a': // Add file path
            fs_path = optarg;
            break;

        case 'f': // File to add
            local_file = optarg;
            break;

        case 'r': // Remove file
            return remove_file(optarg);

        case 'e': // Extract file
            return extract_file(optarg);

        case 'D': // Debug path
            return debug_path(optarg);

        default:
            fprintf(stderr, "Usage: %s [-l] [-a fs_path -f local_file] [-r path] [-e path] [-D path]\n", argv[0]);
            return 1;
        }
    }

    // Handle adding a file if both -a and -f were specified
    if (fs_path != NULL && local_file != NULL)
    {

        return add_file(fs_path, local_file);
    }
    else if (fs_path != NULL || local_file != NULL)
    {
        fprintf(stderr, "Both -a and -f must be specified together\n");
        return 1;
    }

    // Default action if no arguments were provided
    fprintf(stderr, "Usage: %s [-l] [-a fs_path -f local_file] [-r path] [-e path] [-D path]\n", argv[0]);
    return 1;
}
