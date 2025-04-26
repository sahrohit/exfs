#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SEGMENT_SIZE (1024 * 1024) // 1MB segments
#define BLOCK_SIZE 4096            // 4KB blocks
#define INODE_SIZE BLOCK_SIZE      // Each inode is one block
#define DATA_SIZE BLOCK_SIZE       // Each inode is one block

#define MAX_DIRECT_BLOCKS ((INODE_SIZE - 160) / sizeof(uint32_t)) // Rough estimate, adjust based on attributes

// Define inode file index structure and bitmap
#define MAX_INODES (SEGMENT_SIZE / INODE_SIZE) // Maximum inodes in 1MB
#define BITMAP_BYTES (MAX_INODES - 1)          // Size of bitmap in bytes

/* File types */
#define FILE_TYPE_REGULAR 1
#define FILE_TYPE_DIRECTORY 2

/* Segment file name pattern */
#define INODE_SEGMENT_NAME_PATTERN "inodeseg%d"
#define DATA_SEGMENT_NAME_PATTERN "dataseg%d"

typedef struct
{
    uint32_t type;                             // File type (regular or directory)
    uint64_t size;                             // File size in bytes
    uint32_t direct_blocks[MAX_DIRECT_BLOCKS]; // Direct block pointers
    uint32_t single_indirect;                  // Single indirect block
    uint32_t double_indirect;                  // Double indirect block
    // uint32_t triple_indirect;                  // Triple indirect block
    // Add other necessary fields here
} inode_t;

typedef struct
{
    char data[BLOCK_SIZE]; // Data block content
} datablock_t;

// typedef struct
// {
//     uint32_t magic;               // Magic number to identify file
//     uint32_t inode_count;         // Total count of inodes
//     uint32_t used_inodes;         // Number of used inodes
//     uint8_t bitmap[BITMAP_BYTES]; // Bitmap: 1=used, 0=free
// } inode_index_t;

// Create an function to read the inode from a segment file. If the inode number is greater than 255 take divisor as a file name number and take the remainder as the inode number. Read the segment file and read the inode from the file. If the file is not found return -1. If the inode is not found return -2. If the inode is found return 0.
int read_inode(int inode_number, inode_t *inode)
{
    uint8_t bitmap[BITMAP_BYTES];
    FILE *file = NULL;
    char filename[32];
    int segment_num = inode_number / 256; // Calculate segment number
    int inode_index = inode_number % 256; // Calculate inode index

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
    int segment_num = datablock_number / 256;     // Calculate segment number
    int datablock_index = datablock_number % 256; // Calculate datablock index

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

        printf("Checking segment file %s...\n", filename);

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

            printf("Current Bitmap: ");
            for (int j = 0; j < BITMAP_BYTES; j++)
            {
                printf("%u ", bitmap[j]);
            }
            printf("\n");

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

        printf("Checking segment file %s...\n", filename);

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

            printf("Current Bitmap: ");
            for (int j = 0; j < BITMAP_BYTES; j++)
            {
                printf("%u ", bitmap[j]);
            }
            printf("\n");

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

// Function that takes a file path and create a inode for that file and save it to the first available free block in an available segment and then create a datablock for that file and save it to the first available free block in an available segment. Save the datablock index in the inode.direct_blocks[0].
int create_inode_for_file(const char *file_path)
{
    inode_t inode;
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

    // Calculate how many blocks we need
    block_count = (inode.size + BLOCK_SIZE - 1) / BLOCK_SIZE; // Ceiling division
    if (block_count > MAX_DIRECT_BLOCKS)
    {
        fprintf(stderr, "File too large for direct blocks only\n");
        fclose(file);
        return -1;
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

    printf("Total Block Count %d\n", block_count);

    // Print the inode.direct_blocks that was going to be written to the file
    printf("Inode Direct Blocks: ");
    for (int i = 0; i < block_count; i++)
    {
        printf("%u ", inode.direct_blocks[i]);
    }
    printf("\n");

    fclose(file);

    inode.type = FILE_TYPE_REGULAR; // Regular file
    inode.single_indirect = 0;
    inode.double_indirect = 0;
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

int main()
{
    inode_t inode;

    int root_inode_index = create_inode(&inode);

    size_t inode_index = create_inode_for_file("./sample.txt");
    if (inode_index < 0)
    {
        fprintf(stderr, "Failed to create inode for file\n");
        return -1;
    }

    printf("Inode created successfully with index: %zu\n", inode_index);

    inode_t extracted_inode;
    int result = read_inode(inode_index, &extracted_inode);
    if (result < 0)
    {
        fprintf(stderr, "Failed to read inode\n");
        return -1;
    }

    printf("Inode read successfully:\n");
    printf("Type: %u\n", extracted_inode.type);
    printf("Size: %lu\n", extracted_inode.size);
    printf("Direct blocks: ");
    for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
    {
        printf("%u ", extracted_inode.direct_blocks[i]);
    }
    printf("\n");
    printf("Single indirect: %u\n", extracted_inode.single_indirect);
    printf("Double indirect: %u\n", extracted_inode.double_indirect);

    // From the list of direct blocks, read the datablocks and print their contents
    for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
    {
        datablock_t datablock;
        int result = read_datablock(extracted_inode.direct_blocks[i], &datablock);
        if (result < 0)
        {
            fprintf(stderr, "Failed to read datablock\n");
            return -1;
        }

        printf("Datablock %u content: ", extracted_inode.direct_blocks[i]);
        for (int j = 0; j < BLOCK_SIZE; j++)
        {
            printf("%c", datablock.data[j]);
        }
        printf("\n");
    }

    // Read and print the inode and its datablocks from the inodenumber returned by create_inode_for_file

    // uint32_t next = 8;
    // uint8_t bitmap[BITMAP_BYTES];

    // Create a new inode
    // inode_t inode;

    // inode.type = FILE_TYPE_REGULAR; // Regular file
    // inode.size = 0x1024;            // Size in bytes
    // for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
    // {
    //     inode.direct_blocks[i] = i + 1; // Assign block numbers
    // }
    // inode.single_indirect = 0;
    // inode.double_indirect = 0;
    // // inode.triple_indirect = 0;

    // datablock_t data;
    // memset(&data, 5, 4096); // Initialize data block

    // // Create index with bitmap
    // inode_index_t index;
    // index.magic = 0x494E4F44; // "INOD" in hex
    // index.inode_count = MAX_INODES;
    // index.used_inodes = 1; // We're using one inode

    // Initialize bitmap (all zeroes)
    // memset(bitmap, 0, BITMAP_BYTES);

    // // Mark first inode as used (bit 0 set to 1)
    // bitmap[0] = 0x01; // 00000001
    // bitmap[8] = 0x01; // 00000001

    // // Save index and inode to file
    // FILE *file = fopen("inodeseg0", "wb");
    // if (file == NULL)
    // {
    //     perror("Failed to open file");
    //     return -1;
    // }

    // Write index first
    // fwrite(&next, sizeof(next), 1, file);

    // fwrite(&bitmap, sizeof(bitmap), 1, file);

    // Write the inode to the file 255 times
    // for (int i = 0; i < 256; i++)
    // {
    //     create_inode(&inode);
    // }

    // for (int i = 0; i < 256; i++)
    // {
    //     create_datablock(&data);
    // }

    // Set the written variable for the code after the placeholder
    size_t written = 1;

    // create_inode(&inode, bitmap);
    // create_inode(&inode, bitmap);

    // if (written != 1)
    // {
    //     perror("Failed to write inode to file");
    //     fclose(file);
    //     return -1;
    // }

    // fclose(file);
    // printf("Inode and index saved to file successfully.\n");

    // inode_t extracted_inode;
    // datablock_t extracted_datablock;

    // // // uint32_t extracted_next = 1;
    // uint8_t extracted_bitmap[BITMAP_BYTES];
    // // inode_t extracted_inode;

    // FILE *file = fopen("dataseg0", "r+b");
    // if (file == NULL)
    // {
    //     perror("Failed to open file");
    //     return -1;
    // }

    // // Read first inode
    // fseek(file, 0, SEEK_SET);
    // size_t read = fread(&extracted_bitmap, sizeof(extracted_bitmap), 1, file);

    // printf("Inode .\n");
    // printf("Inode Extracted Bitmap: ");
    // for (int i = 0; i < BITMAP_BYTES; i++)
    // {
    //     printf("%u ", extracted_bitmap[i]);
    // }

    // // Read each of the 255 inodes
    // for (int i = 0; i < 255; i++)
    // {
    //     // Position file pointer to the correct inode position
    //     fseek(file, (i + 1) * INODE_SIZE, SEEK_SET);

    //     // Read the inode
    //     read = fread(&extracted_datablock, sizeof(extracted_datablock), 1, file);
    //     if (read != 1)
    //     {
    //         perror("Failed to read inode from file");
    //         fclose(file);
    //         return -1;
    //     }

    //     // Log the inode type
    //     printf("DataBlock %d data0: %u\n", i, extracted_datablock.data[0]);
    // }

    // printf("\nSuccessfully read all inodes.\n");

    // read = fread(&extracted_inode, sizeof(extracted_inode), 1, file);
    // if (read != 1)
    // {
    //     perror("Failed to read inode from file");
    //     fclose(file);
    //     return -1;
    // }

    // fclose(file);

    // // // Display index information
    // // printf("Index read from file successfully.\n");
    // // printf("Next Index: 0x%X\n", extracted_next);
    // // printf("Total inodes: %u\n", read_index.inode_count);
    // // printf("Used inodes: %u\n", read_index.used_inodes);
    // // printf("Bitmap (first byte): 0x%02X\n", extracted_bitmap[2]);

    // // // Display inode information
    // // printf("\nInode read from file successfully.\n");
    // // printf("Type: %u\n", read_inode.type);
    // // printf("Size: %lu\n", read_inode.size);
    // // printf("Direct blocks: ");

    // FILE *data_file = fopen("dataseg0", "r+b");
    // if (file == NULL)
    // {
    //     perror("Failed to open file");
    //     return -1;
    // }

    // // Read first inode
    // fseek(data_file, 0, SEEK_SET);
    // read = fread(&extracted_bitmap, sizeof(extracted_bitmap), 1, data_file);

    // printf("Data .\n");
    // printf("Data Extracted Bitmap: ");
    // for (int i = 0; i < BITMAP_BYTES; i++)
    // {
    //     printf("%u ", extracted_bitmap[i]);
    // }

    // read = fread(&extracted_datablock, sizeof(extracted_datablock), 1, data_file);

    // if (read != 1)
    // {
    //     perror("Failed to read inode from file");
    //     fclose(data_file);
    //     return -1;
    // }

    // // Extract datablock information
    // // printf("\nData block read from file successfully.\n");
    // // printf("Data block content: ");

    // printf("Data .\n");
    // printf("Data Extracted Bitmap: ");
    // for (int i = 0; i < BITMAP_BYTES; i++)
    // {
    //     printf("%u ", extracted_bitmap[i]);
    // }

    // printf("Data block read from file successfully.\n");
    // printf("Data block content: ");
    // for (int i = 0; i < BLOCK_SIZE; i++)
    // {
    //     for (int i = 0; i < 4096; i++)
    //     {

    //         printf("%u ", extracted_datablock.data[i]);
    //     }
    // }
    // printf("\n");

    // // printf("\n Extracted INode Type %X \n", extracted_inode.type);
    // // printf("Extracted INode Size %X \n", extracted_inode.size);
    // // printf("Extracted INode Direct Blocks %X \n", extracted_inode.direct_blocks);
    // // printf("Extracted INode Single Indirect %X \n", extracted_inode.single_indirect);
    // // printf("Extracted INode Double Indirect %X \n", extracted_inode.double_indirect);
    // // // printf("Extracted INode Tripe Indirect %X \n", extracted_inode.triple_indirect);

    // // // printf("\n");
    // // // printf("Single indirect: %u\n", read_inode.single_indirect);
    // // // printf("Double indirect: %u\n", read_inode.double_indirect);
    // // // printf("Triple indirect: %u\n", read_inode.triple_indirect);

    return 0;
}