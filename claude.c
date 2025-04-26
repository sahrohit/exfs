#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>

/* Definitions */
#define SEGMENT_SIZE (1024 * 1024) // 1MB segments
#define BLOCK_SIZE 4096            // 4KB blocks
#define INODE_SIZE BLOCK_SIZE      // Each inode is one block
#define MAX_FILENAME 255           // Max length of filename
#define MAX_PATH 4096              // Max path length

/* File types */
#define FILE_TYPE_REGULAR 1
#define FILE_TYPE_DIRECTORY 2

/* Number of blocks per segment (excluding bitmap) */
#define BLOCKS_PER_DATA_SEGMENT ((SEGMENT_SIZE - (SEGMENT_SIZE / (BLOCK_SIZE * 8))) / BLOCK_SIZE)

/* Directory segment and inode calculations */
#define INODES_PER_SEGMENT ((SEGMENT_SIZE - BLOCK_SIZE) / INODE_SIZE) // One block for bitmap

/* Segment file name pattern */
#define INODE_SEGMENT_NAME_PATTERN "exfs2_inode_segment_%d"
#define DATA_SEGMENT_NAME_PATTERN "exfs2_data_segment_%d"

/* Constants for block pointers */
#define MAX_DIRECT_BLOCKS ((INODE_SIZE - 128) / sizeof(uint32_t)) // Rough estimate, adjust based on attributes
#define POINTERS_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))

/* Special block pointer values */
#define BLOCK_NULL UINT32_MAX // No block assigned

/*
 * Structures
 */

/* Inode structure */
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

/* Directory entry */
typedef struct
{
    char name[MAX_FILENAME]; // Name of the file/directory
    uint32_t inode_number;   // Inode number
} dir_entry_t;

/* Block pointer (for indirect blocks) */
typedef struct
{
    uint32_t pointers[POINTERS_PER_BLOCK];
} block_pointers_t;

/* Segment descriptor */
typedef struct
{
    int segment_number;      // Segment number
    char filename[MAX_PATH]; // Segment file name
    int fd;                  // File descriptor (when opened)
} segment_desc_t;

/* Block address (segment + offset) */
typedef struct
{
    uint32_t segment_num; // Segment number
    uint32_t block_num;   // Block number within segment
} block_addr_t;

/* Global variables */
int current_inode_segments = 0; // Number of inode segments
int current_data_segments = 0;  // Number of data segments

/*
 * Function prototypes
 */

/* Core file system operations */
int init_file_system();
int list_directory(const char *path);
int add_file(const char *fs_path, const char *local_file);
int remove_file(const char *path);
int extract_file(const char *path);
int debug_path(const char *path);

/* Inode operations */
uint32_t allocate_inode();
void free_inode(uint32_t inode_num);
int read_inode(uint32_t inode_num, inode_t *inode);
int write_inode(uint32_t inode_num, const inode_t *inode);
uint32_t lookup_path(const char *path);
uint32_t lookup_path_component(uint32_t dir_inode, const char *name);
int create_directory(uint32_t parent_inode, const char *name);

/* Block operations */
uint32_t allocate_block();
void free_block(uint32_t block_num);
int read_block(uint32_t block_num, void *buffer);
int write_block(uint32_t block_num, const void *buffer);
block_addr_t translate_block_address(uint32_t block_num);

/* Segment operations */
int init_inode_segment(int segment_num);
int init_data_segment(int segment_num);
int open_segment(const char *filename, int create);
char *get_inode_segment_name(int segment_num);
char *get_data_segment_name(int segment_num);

/* Directory operations */
int add_directory_entry(uint32_t dir_inode, const char *name, uint32_t entry_inode);
int remove_directory_entry(uint32_t dir_inode, const char *name);
int read_directory_entries(uint32_t dir_inode, dir_entry_t **entries, int *count);

/* Helper functions */
char *get_basename(const char *path);
char *get_parent_path(const char *path);
void ensure_path_exists(const char *path);

/* Bitmap operations */
void set_bit(uint8_t *bitmap, int bit_num);
void clear_bit(uint8_t *bitmap, int bit_num);
int test_bit(const uint8_t *bitmap, int bit_num);
int find_first_zero_bit(const uint8_t *bitmap, int size);

/*
 * Implementation
 */

int main(int argc, char *argv[])
{
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
            return list_directory("/");

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

/*
 * Initialize file system - checks for existing segments or creates initial segments
 */
int init_file_system()
{
    struct stat st;
    char segment_name[MAX_PATH];
    int i = 0;

    // // Count existing inode segments
    // while (1)
    // {
    //     sprintf(segment_name, INODE_SEGMENT_NAME_PATTERN, i);
    //     if (stat(segment_name, &st) == 0)
    //     {
    //         current_inode_segments++;
    //         i++;
    //     }
    //     else
    //     {
    //         break;
    //     }
    // }

    // Count existing inode segments; we only trust ones at exactly SEGMENT_SIZE bytes
    while (1)
    {
        sprintf(segment_name, INODE_SEGMENT_NAME_PATTERN, i);
        if (stat(segment_name, &st) == 0)
        {
            if (st.st_size == SEGMENT_SIZE)
            {
                current_inode_segments++;
                i++;
                continue;
            }
            // truncated or corrupted segment: throw it away
            fprintf(stderr, "Warning: inode segment %s is %ld bytes, reinitializing\n",
                    segment_name, (long)st.st_size);
            unlink(segment_name);
        }
        break;
    }

    // Count existing data segments
    i = 0;
    while (1)
    {
        sprintf(segment_name, DATA_SEGMENT_NAME_PATTERN, i);
        if (stat(segment_name, &st) == 0)
        {
            if (st.st_size == SEGMENT_SIZE)
            {
                current_data_segments++;
                i++;
                continue;
            }
            fprintf(stderr, "Warning: data segment %s is %ld bytes, reinitializing\n",
                    segment_name, (long)st.st_size);
            unlink(segment_name);
        }
        break;
    }

    // If no segments exist, create initial segments
    if (current_inode_segments == 0 || current_data_segments == 0)
    {
        printf("No existing file system found. Creating new file system...\n");

        // Create first inode segment
        if (init_inode_segment(0) != 0)
        {
            fprintf(stderr, "Failed to create initial inode segment\n");
            return -1;
        }
        current_inode_segments = 1;

        // Create first data segment
        if (init_data_segment(0) != 0)
        {
            fprintf(stderr, "Failed to create initial data segment\n");
            return -1;
        }
        current_data_segments = 1;

        // Create root directory
        //
        // -- Seed the root directory properly, with "." and ".." --
        //
        inode_t root_inode;
        memset(&root_inode, 0, sizeof(inode_t));
        root_inode.type = FILE_TYPE_DIRECTORY;

        // Allocate one block to hold "." and ".."
        uint32_t blk = allocate_block();
        if (blk == BLOCK_NULL)
        {
            fprintf(stderr, "Failed to allocate block for root dir\n");
            return -1;
        }

        // Initialize that block:
        {
            uint8_t buf[BLOCK_SIZE];
            memset(buf, 0, BLOCK_SIZE);
            dir_entry_t *ents = (dir_entry_t *)buf;

            // "."  → inode 0
            strncpy(ents[0].name, ".", MAX_FILENAME);
            ents[0].inode_number = 0;
            // ".." → inode 0 (parent of root is itself)
            strncpy(ents[1].name, "..", MAX_FILENAME);
            ents[1].inode_number = 0;

            if (write_block(blk, buf) != 0)
            {
                fprintf(stderr, "Failed to write root dir block\n");
                return -1;
            }
        }

        // Hook it into the inode
        root_inode.direct_blocks[0] = blk;
        root_inode.size = 2 * sizeof(dir_entry_t);

        // Finally write the root inode
        if (write_inode(0, &root_inode) != 0)
        {
            fprintf(stderr, "Failed to write root inode\n");
            return -1;
        }

        printf("File system initialized successfully\n");
    }

    return 0;
}

/*
 * List directory contents recursively
 */
int list_directory(const char *path)
{
    uint32_t dir_inode = lookup_path(path);
    if (dir_inode == 0 && strcmp(path, "/") != 0)
    {
        fprintf(stderr, "Directory not found: %s\n", path);
        return -1;
    }

    // Read inode for the directory
    inode_t inode;
    if (read_inode(dir_inode, &inode) != 0)
    {
        fprintf(stderr, "Failed to read directory inode\n");
        return -1;
    }

    // Check if it's actually a directory
    if (inode.type != FILE_TYPE_DIRECTORY)
    {
        fprintf(stderr, "%s is not a directory\n", path);
        return -1;
    }

    // Read directory entries
    dir_entry_t *entries = NULL;
    int count = 0;
    if (read_directory_entries(dir_inode, &entries, &count) != 0)
    {
        fprintf(stderr, "Failed to read directory entries\n");
        return -1;
    }

    // Print directory contents
    printf("Directory listing for %s:\n", path);
    printf("-------------------------\n");
    for (int i = 0; i < count; i++)
    {
        // Read the entry's inode to determine if it's a file or directory
        inode_t entry_inode;
        if (read_inode(entries[i].inode_number, &entry_inode) != 0)
        {
            fprintf(stderr, "Failed to read inode for %s\n", entries[i].name);
            continue;
        }

        // Print entry information
        printf("%s%s\n", entries[i].name, entry_inode.type == FILE_TYPE_DIRECTORY ? "/" : "");
    }

    // Clean up
    free(entries);
    return 0;
}

/*
 * Add a file to the file system
 */
int add_file(const char *fs_path, const char *local_file)
{
    // Check if the local file exists and can be opened
    FILE *local_fp = fopen(local_file, "rb");
    if (local_fp == NULL)
    {
        perror("Failed to open local file");
        return -1;
    }

    // Get the file size
    fseek(local_fp, 0, SEEK_END);
    long file_size = ftell(local_fp);
    fseek(local_fp, 0, SEEK_SET);

    // Get parent directory path and file name
    char *parent_path = get_parent_path(fs_path);
    char *file_name = get_basename(fs_path);

    // Ensure parent directory exists
    ensure_path_exists(parent_path);

    // Look up parent directory inode
    uint32_t parent_inode = lookup_path(parent_path);
    if (parent_inode == 0 && strcmp(parent_path, "/") != 0)
    {
        fprintf(stderr, "Parent directory not found: %s\n", parent_path);
        fclose(local_fp);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // Check if a file with the same name already exists
    uint32_t existing_inode = lookup_path_component(parent_inode, file_name);
    if (existing_inode != 0)
    {
        fprintf(stderr, "File already exists: %s\n", fs_path);
        fclose(local_fp);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // Allocate an inode for the new file
    uint32_t file_inode = allocate_inode();
    if (file_inode == 0)
    {
        fprintf(stderr, "Failed to allocate inode for file\n");
        fclose(local_fp);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // Initialize the inode
    inode_t inode;
    memset(&inode, 0, sizeof(inode_t));
    inode.type = FILE_TYPE_REGULAR;
    inode.size = file_size;

    // Read file data in blocks and write to the file system
    uint8_t buffer[BLOCK_SIZE];
    uint32_t block_index = 0;
    uint32_t bytes_read = 0;

    while (bytes_read < file_size)
    {
        // Read a block from the local file
        size_t read_size = fread(buffer, 1, BLOCK_SIZE, local_fp);
        if (read_size == 0)
        {
            if (ferror(local_fp))
            {
                perror("Error reading local file");
                // TODO: Clean up allocated blocks and inode
                fclose(local_fp);
                free(parent_path);
                free(file_name);
                return -1;
            }
            break; // End of file
        }

        // Allocate a block for the file data
        uint32_t block_num = allocate_block();
        if (block_num == BLOCK_NULL)
        {
            fprintf(stderr, "Failed to allocate block for file data\n");
            // TODO: Clean up allocated blocks and inode
            fclose(local_fp);
            free(parent_path);
            free(file_name);
            return -1;
        }

        // Write data to the block
        if (write_block(block_num, buffer) != 0)
        {
            fprintf(stderr, "Failed to write block data\n");
            // TODO: Clean up allocated blocks and inode
            fclose(local_fp);
            free(parent_path);
            free(file_name);
            return -1;
        }

        // Update inode block pointers
        if (block_index < MAX_DIRECT_BLOCKS)
        {
            // Direct block pointer
            inode.direct_blocks[block_index] = block_num;
        }
        else if (block_index < MAX_DIRECT_BLOCKS + POINTERS_PER_BLOCK)
        {
            // Single indirect block pointer
            if (inode.single_indirect == BLOCK_NULL)
            {
                inode.single_indirect = allocate_block();
                if (inode.single_indirect == BLOCK_NULL)
                {
                    fprintf(stderr, "Failed to allocate indirect block\n");
                    // TODO: Clean up allocated blocks and inode
                    fclose(local_fp);
                    free(parent_path);
                    free(file_name);
                    return -1;
                }

                // Initialize indirect block
                block_pointers_t indirect_block;
                memset(&indirect_block, 0, sizeof(block_pointers_t));
                if (write_block(inode.single_indirect, &indirect_block) != 0)
                {
                    fprintf(stderr, "Failed to write indirect block\n");
                    // TODO: Clean up allocated blocks and inode
                    fclose(local_fp);
                    free(parent_path);
                    free(file_name);
                    return -1;
                }
            }

            // Update indirect block
            block_pointers_t indirect_block;
            if (read_block(inode.single_indirect, &indirect_block) != 0)
            {
                fprintf(stderr, "Failed to read indirect block\n");
                // TODO: Clean up allocated blocks and inode
                fclose(local_fp);
                free(parent_path);
                free(file_name);
                return -1;
            }

            indirect_block.pointers[block_index - MAX_DIRECT_BLOCKS] = block_num;

            if (write_block(inode.single_indirect, &indirect_block) != 0)
            {
                fprintf(stderr, "Failed to update indirect block\n");
                // TODO: Clean up allocated blocks and inode
                fclose(local_fp);
                free(parent_path);
                free(file_name);
                return -1;
            }
        }
        else
        {
            // Double and triple indirect blocks not implemented yet
            fprintf(stderr, "File too large: double and triple indirect blocks not supported yet\n");
            // TODO: Clean up allocated blocks and inode
            fclose(local_fp);
            free(parent_path);
            free(file_name);
            return -1;
        }

        block_index++;
        bytes_read += read_size;
    }

    // Write the file inode
    if (write_inode(file_inode, &inode) != 0)
    {
        fprintf(stderr, "Failed to write file inode\n");
        // TODO: Clean up allocated blocks and inode
        fclose(local_fp);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // Add directory entry to parent directory
    if (add_directory_entry(parent_inode, file_name, file_inode) != 0)
    {
        fprintf(stderr, "Failed to add directory entry\n");
        // TODO: Clean up allocated blocks and inode
        fclose(local_fp);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // Clean up
    fclose(local_fp);
    free(parent_path);
    free(file_name);

    printf("File added successfully: %s (%ld bytes)\n", fs_path, file_size);
    return 0;
}

/*
 * Remove a file or directory from the file system
 */
int remove_file(const char *path)
{
    // Handle root directory as a special case
    if (strcmp(path, "/") == 0)
    {
        fprintf(stderr, "Cannot remove root directory\n");
        return -1;
    }

    // Get parent directory path and file name
    char *parent_path = get_parent_path(path);
    char *file_name = get_basename(path);

    // Look up parent directory inode
    uint32_t parent_inode = lookup_path(parent_path);
    if (parent_inode == 0 && strcmp(parent_path, "/") != 0)
    {
        fprintf(stderr, "Parent directory not found: %s\n", parent_path);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // Look up the inode for the file/directory to remove
    uint32_t target_inode = lookup_path_component(parent_inode, file_name);
    if (target_inode == 0)
    {
        fprintf(stderr, "File or directory not found: %s\n", path);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // Read the target inode
    inode_t inode;
    if (read_inode(target_inode, &inode) != 0)
    {
        fprintf(stderr, "Failed to read inode for %s\n", path);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // If it's a directory, ensure it's empty
    if (inode.type == FILE_TYPE_DIRECTORY)
    {
        dir_entry_t *entries = NULL;
        int count = 0;
        if (read_directory_entries(target_inode, &entries, &count) != 0)
        {
            fprintf(stderr, "Failed to read directory entries for %s\n", path);
            free(parent_path);
            free(file_name);
            return -1;
        }

        if (count > 0)
        {
            fprintf(stderr, "Directory not empty: %s\n", path);
            free(entries);
            free(parent_path);
            free(file_name);
            return -1;
        }

        free(entries);
    }

    // Free all blocks used by the file/directory
    // Start with direct blocks
    for (int i = 0; i < MAX_DIRECT_BLOCKS; i++)
    {
        if (inode.direct_blocks[i] != BLOCK_NULL)
        {
            free_block(inode.direct_blocks[i]);
        }
    }

    // Free single indirect block and its contents
    if (inode.single_indirect != BLOCK_NULL)
    {
        block_pointers_t indirect_block;
        if (read_block(inode.single_indirect, &indirect_block) == 0)
        {
            for (int i = 0; i < POINTERS_PER_BLOCK; i++)
            {
                if (indirect_block.pointers[i] != BLOCK_NULL)
                {
                    free_block(indirect_block.pointers[i]);
                }
            }
        }
        free_block(inode.single_indirect);
    }

    // Free double indirect block and its contents (not fully implemented)
    if (inode.double_indirect != BLOCK_NULL)
    {
        // TODO: Implement double indirect blocks cleanup
        free_block(inode.double_indirect);
    }

    // Free triple indirect block and its contents (not fully implemented)
    if (inode.triple_indirect != BLOCK_NULL)
    {
        // TODO: Implement triple indirect blocks cleanup
        free_block(inode.triple_indirect);
    }

    // Remove directory entry from parent directory
    if (remove_directory_entry(parent_inode, file_name) != 0)
    {
        fprintf(stderr, "Failed to remove directory entry for %s\n", path);
        free(parent_path);
        free(file_name);
        return -1;
    }

    // Free the inode
    free_inode(target_inode);

    // Clean up
    free(parent_path);
    free(file_name);

    printf("Successfully removed %s\n", path);
    return 0;
}

/*
 * Extract file contents to stdout
 */
int extract_file(const char *path)
{
    // Look up the file inode
    uint32_t file_inode = lookup_path(path);
    if (file_inode == 0)
    {
        fprintf(stderr, "File not found: %s\n", path);
        return -1;
    }

    // Read the file inode
    inode_t inode;
    if (read_inode(file_inode, &inode) != 0)
    {
        fprintf(stderr, "Failed to read inode for %s\n", path);
        return -1;
    }

    // Check if it's a regular file
    if (inode.type != FILE_TYPE_REGULAR)
    {
        fprintf(stderr, "%s is not a regular file\n", path);
        return -1;
    }

    // Extract file contents block by block
    uint8_t buffer[BLOCK_SIZE];
    uint64_t bytes_remaining = inode.size;
    uint32_t block_index = 0;

    // Extract from direct blocks
    while (block_index < MAX_DIRECT_BLOCKS && bytes_remaining > 0)
    {
        uint32_t block_num = inode.direct_blocks[block_index];
        if (block_num == BLOCK_NULL)
        {
            break;
        }

        // Read block data
        if (read_block(block_num, buffer) != 0)
        {
            fprintf(stderr, "Failed to read block %u\n", block_num);
            return -1;
        }

        // Write to stdout
        size_t bytes_to_write = bytes_remaining < BLOCK_SIZE ? bytes_remaining : BLOCK_SIZE;
        if (fwrite(buffer, 1, bytes_to_write, stdout) != bytes_to_write)
        {
            perror("Failed to write to stdout");
            return -1;
        }

        bytes_remaining -= bytes_to_write;
        block_index++;
    }

    // Extract from single indirect block
    if (bytes_remaining > 0 && inode.single_indirect != BLOCK_NULL)
    {
        block_pointers_t indirect_block;
        if (read_block(inode.single_indirect, &indirect_block) != 0)
        {
            fprintf(stderr, "Failed to read indirect block\n");
            return -1;
        }

        for (int i = 0; i < POINTERS_PER_BLOCK && bytes_remaining > 0; i++)
        {
            uint32_t block_num = indirect_block.pointers[i];
            if (block_num == BLOCK_NULL)
            {
                break;
            }

            // Read block data
            if (read_block(block_num, buffer) != 0)
            {
                fprintf(stderr, "Failed to read block %u\n", block_num);
                return -1;
            }

            // Write to stdout
            size_t bytes_to_write = bytes_remaining < BLOCK_SIZE ? bytes_remaining : BLOCK_SIZE;
            if (fwrite(buffer, 1, bytes_to_write, stdout) != bytes_to_write)
            {
                perror("Failed to write to stdout");
                return -1;
            }

            bytes_remaining -= bytes_to_write;
        }
    }

    // Double and triple indirect blocks not implemented yet
    if (bytes_remaining > 0)
    {
        fprintf(stderr, "Warning: File extraction incomplete, some blocks not processed\n");
    }

    return 0;
}

/*
 * Debug path - show inode information for each component
 */
int debug_path(const char *path)
{
    // Handle empty path
    if (path == NULL || *path == '\0')
    {
        fprintf(stderr, "Empty path\n");
        return -1;
    }

    // Handle root directory as a special case
    if (strcmp(path, "/") == 0)
    {
        printf("Path component: / (inode: 0)\n");

        inode_t root_inode;
        if (read_inode(0, &root_inode) != 0)
        {
            fprintf(stderr, "Failed to read root inode\n");
            return -1;
        }

        printf("  Type: %s\n", root_inode.type == FILE_TYPE_DIRECTORY ? "Directory" : "File");
        printf("  Size: %lu bytes\n", root_inode.size);

        return 0;
    }

    // Parse path component by component
    char *path_copy = strdup(path);
    if (path_copy == NULL)
    {
        perror("Failed to allocate memory for path copy");
        return -1;
    }

    // Start from root
    uint32_t current_inode = 0;
    inode_t inode;

    // Skip leading slash if present
    char *path_ptr = path_copy;
    if (*path_ptr == '/')
    {
        path_ptr++;
    }

    // Tokenize path
    char *component = strtok(path_ptr, "/");
    uint32_t parent_inode = 0;

    printf("Path component: / (inode: 0)\n");

    while (component != NULL)
    {
        parent_inode = current_inode;
        current_inode = lookup_path_component(parent_inode, component);

        if (current_inode == 0)
        {
            printf("Path component: %s (not found)\n", component);
            free(path_copy);
            return -1;
        }

        if (read_inode(current_inode, &inode) != 0)
        {
            fprintf(stderr, "Failed to read inode for %s\n", component);
            free(path_copy);
            return -1;
        }

        printf("Path component: %s (inode: %u)\n", component, current_inode);
        printf("  Type: %s\n", inode.type == FILE_TYPE_DIRECTORY ? "Directory" : "File");
        printf("  Size: %lu bytes\n", inode.size);

        // Print block information
        printf("  Direct blocks:");
        for (int i = 0; i < MAX_DIRECT_BLOCKS && inode.direct_blocks[i] != BLOCK_NULL; i++)
        {
            printf(" %u", inode.direct_blocks[i]);
        }
        printf("\n");

        if (inode.single_indirect != BLOCK_NULL)
        {
            printf("  Single indirect block: %u\n", inode.single_indirect);
        }

        if (inode.double_indirect != BLOCK_NULL)
        {
            printf("  Double indirect block: %u\n", inode.double_indirect);
        }

        if (inode.triple_indirect != BLOCK_NULL)
        {
            printf("  Triple indirect block: %u\n", inode.triple_indirect);
        }

        component = strtok(NULL, "/");
    }

    free(path_copy);
    return 0;
}

/*
 * Allocate an inode from the inode segments
 */
uint32_t allocate_inode()
{
    uint8_t bitmap[BLOCK_SIZE];

    // Search through existing inode segments for a free inode
    for (int segment = 0; segment < current_inode_segments; segment++)
    {
        char *segment_name = get_inode_segment_name(segment);
        int fd = open(segment_name, O_RDWR);
        if (fd < 0)
        {
            perror("Failed to open inode segment");
            free(segment_name);
            continue;
        }

        // Read the bitmap block
        if (read(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
        {
            perror("Failed to read inode bitmap");
            close(fd);
            free(segment_name);
            continue;
        }

        // Find first free inode
        int bit_index = find_first_zero_bit(bitmap, INODES_PER_SEGMENT);
        if (bit_index >= 0)
        {
            // Mark inode as used
            set_bit(bitmap, bit_index);

            // Write updated bitmap back to disk
            lseek(fd, 0, SEEK_SET);
            if (write(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
            {
                perror("Failed to write updated inode bitmap");
                close(fd);
                free(segment_name);
                continue;
            }

            close(fd);
            free(segment_name);

            // Calculate global inode number
            return segment * INODES_PER_SEGMENT + bit_index;
        }

        close(fd);
        free(segment_name);
    }

    // No free inodes found, create a new segment
    int new_segment = current_inode_segments;
    if (init_inode_segment(new_segment) != 0)
    {
        fprintf(stderr, "Failed to create new inode segment\n");
        return 0;
    }

    current_inode_segments++;

    // Allocate first inode from new segment (skip bit 0 in first segment which is for root)
    int bit_index = (new_segment == 0) ? 1 : 0;

    char *segment_name = get_inode_segment_name(new_segment);
    int fd = open(segment_name, O_RDWR);
    if (fd < 0)
    {
        perror("Failed to open new inode segment");
        free(segment_name);
        return 0;
    }

    // Read the bitmap block
    if (read(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
    {
        perror("Failed to read inode bitmap");
        close(fd);
        free(segment_name);
        return 0;
    }

    // Mark inode as used
    set_bit(bitmap, bit_index);

    // Write updated bitmap back to disk
    lseek(fd, 0, SEEK_SET);
    if (write(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
    {
        perror("Failed to write updated inode bitmap");
        close(fd);
        free(segment_name);
        return 0;
    }

    close(fd);
    free(segment_name);

    // Calculate global inode number
    return new_segment * INODES_PER_SEGMENT + bit_index;
}

/*
 * Free an inode
 */
void free_inode(uint32_t inode_num)
{
    // Calculate which segment contains the inode
    int segment = inode_num / INODES_PER_SEGMENT;
    int bit_index = inode_num % INODES_PER_SEGMENT;

    // Don't free inode 0 (root directory)
    if (inode_num == 0)
    {
        fprintf(stderr, "Cannot free root inode\n");
        return;
    }

    // Check if segment exists
    if (segment >= current_inode_segments)
    {
        fprintf(stderr, "Invalid inode number: %u\n", inode_num);
        return;
    }

    char *segment_name = get_inode_segment_name(segment);
    int fd = open(segment_name, O_RDWR);
    if (fd < 0)
    {
        perror("Failed to open inode segment");
        free(segment_name);
        return;
    }

    // Read the bitmap block
    uint8_t bitmap[BLOCK_SIZE];
    if (read(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
    {
        perror("Failed to read inode bitmap");
        close(fd);
        free(segment_name);
        return;
    }

    // Mark inode as free
    clear_bit(bitmap, bit_index);

    // Write updated bitmap back to disk
    lseek(fd, 0, SEEK_SET);
    if (write(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
    {
        perror("Failed to write updated inode bitmap");
        close(fd);
        free(segment_name);
        return;
    }

    close(fd);
    free(segment_name);
}

/*
 * Read an inode from its segment
 */
/*
 * Read an inode from its segment
 */
int read_inode(uint32_t inode_num, inode_t *inode)
{
    // Calculate which segment contains the inode
    int segment = inode_num / INODES_PER_SEGMENT;
    int index = inode_num % INODES_PER_SEGMENT;

    // Check if segment exists
    if (segment >= current_inode_segments)
    {
        fprintf(stderr, "Invalid inode number: %u\n", inode_num);
        return -1;
    }

    char *segment_name = get_inode_segment_name(segment);
    int fd = open(segment_name, O_RDONLY);
    if (fd < 0)
    {
        perror("Failed to open inode segment");
        free(segment_name);
        return -1;
    }

    // Calculate offset for the inode (skip bitmap block)
    off_t offset = BLOCK_SIZE + index * INODE_SIZE;

    // Seek to the inode's position
    if (lseek(fd, offset, SEEK_SET) != offset)
    {
        perror("Failed to seek to inode position");
        close(fd);
        free(segment_name);
        return -1;
    }

    // Read the inode
    if (read(fd, inode, sizeof(inode_t)) != sizeof(inode_t))
    {
        perror("Failed to read inode");
        close(fd);
        free(segment_name);
        return -1;
    }

    close(fd);
    free(segment_name);
    return 0;
}

/*
 * Write an inode to its segment
 */
int write_inode(uint32_t inode_num, const inode_t *inode)
{
    // Calculate which segment contains the inode
    int segment = inode_num / INODES_PER_SEGMENT;
    int index = inode_num % INODES_PER_SEGMENT;

    // Check if segment exists
    if (segment >= current_inode_segments)
    {
        fprintf(stderr, "Invalid inode number: %u\n", inode_num);
        return -1;
    }

    char *segment_name = get_inode_segment_name(segment);
    int fd = open(segment_name, O_RDWR);
    if (fd < 0)
    {
        perror("Failed to open inode segment");
        free(segment_name);
        return -1;
    }

    // Calculate offset for the inode (skip bitmap block)
    off_t offset = BLOCK_SIZE + index * INODE_SIZE;

    // Seek to the inode's position
    if (lseek(fd, offset, SEEK_SET) != offset)
    {
        perror("Failed to seek to inode position");
        close(fd);
        free(segment_name);
        return -1;
    }

    // Write the inode
    if (write(fd, inode, sizeof(inode_t)) != sizeof(inode_t))
    {
        perror("Failed to write inode");
        close(fd);
        free(segment_name);
        return -1;
    }

    close(fd);
    free(segment_name);
    return 0;
}

/*
 * Look up a path and return its inode number
 */
uint32_t lookup_path(const char *path)
{
    // Handle empty path
    if (path == NULL || *path == '\0')
    {
        return 0;
    }

    // Handle root directory as a special case
    if (strcmp(path, "/") == 0)
    {
        return 0; // Root inode
    }

    // Parse path component by component
    char *path_copy = strdup(path);
    if (path_copy == NULL)
    {
        perror("Failed to allocate memory for path copy");
        return 0;
    }

    // Start from root
    uint32_t current_inode = 0;

    // Skip leading slash if present
    char *path_ptr = path_copy;
    if (*path_ptr == '/')
    {
        path_ptr++;
    }

    // Handle empty path after slash
    if (*path_ptr == '\0')
    {
        free(path_copy);
        return 0; // Root inode
    }

    // Tokenize path
    char *saveptr;
    char *component = strtok_r(path_ptr, "/", &saveptr);

    while (component != NULL)
    {
        current_inode = lookup_path_component(current_inode, component);

        if (current_inode == 0)
        {
            // Path component not found
            free(path_copy);
            return 0;
        }

        component = strtok_r(NULL, "/", &saveptr);
    }

    free(path_copy);
    return current_inode;
}

/*
 * Allocate a block from the data segments
 */
/**
 * Allocate a block from the data segments
 */
uint32_t allocate_block()
{
    uint8_t bitmap[BLOCK_SIZE];
    int segment_num;
    char segment_filename[MAX_PATH];
    int fd;

    // Search existing data segments for a free block
    for (segment_num = 0; segment_num < current_data_segments; segment_num++)
    {
        // Get segment filename
        snprintf(segment_filename, MAX_PATH, DATA_SEGMENT_NAME_PATTERN, segment_num);

        // Open the segment file
        fd = open(segment_filename, O_RDWR);
        if (fd < 0)
        {
            perror("Failed to open data segment file");
            continue; // Try the next segment
        }

        // Read the bitmap
        if (read(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
        {
            perror("Failed to read bitmap");
            close(fd);
            continue; // Try the next segment
        }

        // Find first free block in this segment
        int bit_pos = find_first_zero_bit(bitmap, BLOCKS_PER_DATA_SEGMENT);
        if (bit_pos >= 0 && bit_pos < BLOCKS_PER_DATA_SEGMENT)
        {
            // Mark block as used
            set_bit(bitmap, bit_pos);

            // Write updated bitmap back to disk
            lseek(fd, 0, SEEK_SET);
            if (write(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
            {
                perror("Failed to write bitmap");
                close(fd);
                continue; // Try the next segment
            }

            close(fd);

            // Calculate global block number
            uint32_t block_num = segment_num * BLOCKS_PER_DATA_SEGMENT + bit_pos;
            return block_num;
        }

        close(fd);
    }

    // No free blocks found, create a new data segment
    segment_num = current_data_segments;
    if (init_data_segment(segment_num) != 0)
    {
        fprintf(stderr, "Failed to initialize new data segment\n");
        return BLOCK_NULL;
    }

    // Mark the first block as used in the new segment
    snprintf(segment_filename, MAX_PATH, DATA_SEGMENT_NAME_PATTERN, segment_num);
    fd = open(segment_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Failed to open new data segment file");
        return BLOCK_NULL;
    }

    // Read the bitmap (should be mostly zeros as it's a new segment)
    if (read(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
    {
        perror("Failed to read bitmap from new segment");
        close(fd);
        return BLOCK_NULL;
    }

    // Mark first block as used
    set_bit(bitmap, 0);

    // Write updated bitmap back to disk
    lseek(fd, 0, SEEK_SET);
    if (write(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
    {
        perror("Failed to write bitmap to new segment");
        close(fd);
        return BLOCK_NULL;
    }

    close(fd);
    current_data_segments++; // Increment the count of data segments

    // Return the block number
    return segment_num * BLOCKS_PER_DATA_SEGMENT + 0;
}

/**
 * Free a block
 */
void free_block(uint32_t block_num)
{
    // Translate block number to segment and offset
    block_addr_t addr = translate_block_address(block_num);
    uint8_t bitmap[BLOCK_SIZE];
    char segment_filename[MAX_PATH];
    int fd;

    // Calculate the bit position in the bitmap (subtract 1 to account for bitmap block)
    int bit_pos = addr.block_num - 1;

    // Get segment filename
    snprintf(segment_filename, MAX_PATH, DATA_SEGMENT_NAME_PATTERN, addr.segment_num);

    // Open the segment file
    fd = open(segment_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Failed to open data segment file for freeing block");
        return;
    }

    // Read the bitmap
    if (read(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
    {
        perror("Failed to read bitmap for freeing block");
        close(fd);
        return;
    }

    // Mark block as free
    clear_bit(bitmap, bit_pos);

    // Write updated bitmap back to disk
    lseek(fd, 0, SEEK_SET);
    if (write(fd, bitmap, BLOCK_SIZE) != BLOCK_SIZE)
    {
        perror("Failed to write bitmap for freeing block");
        close(fd);
        return;
    }

    close(fd);
}

/**
 * Read a block's data
 */
int read_block(uint32_t block_num, void *buffer)
{
    // Translate block number to segment and offset
    block_addr_t addr = translate_block_address(block_num);
    char segment_filename[MAX_PATH];
    int fd;

    // Get segment filename
    snprintf(segment_filename, MAX_PATH, DATA_SEGMENT_NAME_PATTERN, addr.segment_num);

    // Open the segment file
    fd = open(segment_filename, O_RDONLY);
    if (fd < 0)
    {
        perror("Failed to open data segment file for reading");
        return -1;
    }

    // Seek to the block's position
    // Each block is BLOCK_SIZE bytes, and the first block is the bitmap
    off_t offset = addr.block_num * BLOCK_SIZE;
    if (lseek(fd, offset, SEEK_SET) != offset)
    {
        perror("Failed to seek to block position for reading");
        close(fd);
        return -1;
    }

    // Read the block data
    ssize_t bytes_read = read(fd, buffer, BLOCK_SIZE);
    close(fd);

    if (bytes_read != BLOCK_SIZE)
    {
        perror("Failed to read complete block");
        return -1;
    }

    return 0;
}

/**
 * Write data to a block
 */
int write_block(uint32_t block_num, const void *buffer)
{
    // Translate block number to segment and offset
    block_addr_t addr = translate_block_address(block_num);
    char segment_filename[MAX_PATH];
    int fd;

    // Get segment filename
    snprintf(segment_filename, MAX_PATH, DATA_SEGMENT_NAME_PATTERN, addr.segment_num);

    // Open the segment file
    fd = open(segment_filename, O_RDWR);
    if (fd < 0)
    {
        perror("Failed to open data segment file for writing");
        return -1;
    }

    // Seek to the block's position
    // Each block is BLOCK_SIZE bytes, and the first block is the bitmap
    off_t offset = addr.block_num * BLOCK_SIZE;
    if (lseek(fd, offset, SEEK_SET) != offset)
    {
        perror("Failed to seek to block position for writing");
        close(fd);
        return -1;
    }

    // Write the block data
    ssize_t bytes_written = write(fd, buffer, BLOCK_SIZE);
    close(fd);

    if (bytes_written != BLOCK_SIZE)
    {
        perror("Failed to write complete block");
        return -1;
    }

    return 0;
}

/*
 * Initialize a new inode segment
 */
int init_inode_segment(int segment_num)
{
    char *segment_name = get_inode_segment_name(segment_num);
    int fd = open(segment_name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        perror("Failed to create inode segment");
        free(segment_name);
        return -1;
    }

    // Allocate memory for the segment data
    uint8_t *segment_data = (uint8_t *)calloc(1, SEGMENT_SIZE);
    if (segment_data == NULL)
    {
        perror("Failed to allocate memory for segment data");
        close(fd);
        free(segment_name);
        return -1;
    }

    // Write the segment data (all zeroed out)
    if (write(fd, segment_data, SEGMENT_SIZE) != SEGMENT_SIZE)
    {
        perror("Failed to write segment data");
        free(segment_data);
        close(fd);
        free(segment_name);
        return -1;
    }

    // Mark inode 0 as used if this is the first segment
    if (segment_num == 0)
    {
        lseek(fd, 0, SEEK_SET);
        uint8_t bitmap = 1; // Mark first inode (root directory) as used
        if (write(fd, &bitmap, 1) != 1)
        {
            perror("Failed to mark root inode as used");
            free(segment_data);
            close(fd);
            free(segment_name);
            return -1;
        }
    }

    free(segment_data);
    close(fd);
    free(segment_name);
    return 0;
}

/*
 * Initialize a new data segment
 */
int init_data_segment(int segment_num)
{
    char *segment_name = get_data_segment_name(segment_num);
    int fd = open(segment_name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        perror("Failed to create data segment");
        free(segment_name);
        return -1;
    }

    // Allocate memory for the segment data
    uint8_t *segment_data = (uint8_t *)calloc(1, SEGMENT_SIZE);
    if (segment_data == NULL)
    {
        perror("Failed to allocate memory for segment data");
        close(fd);
        free(segment_name);
        return -1;
    }

    // Write the segment data (all zeroed out)
    if (write(fd, segment_data, SEGMENT_SIZE) != SEGMENT_SIZE)
    {
        perror("Failed to write segment data");
        free(segment_data);
        close(fd);
        free(segment_name);
        return -1;
    }

    free(segment_data);
    close(fd);
    free(segment_name);
    return 0;
}

/*
 * Get the name of an inode segment
 */
char *get_inode_segment_name(int segment_num)
{
    char *name = (char *)malloc(MAX_PATH);
    if (name != NULL)
    {
        sprintf(name, INODE_SEGMENT_NAME_PATTERN, segment_num);
    }
    return name;
}

/*
 * Get the name of a data segment
 */
char *get_data_segment_name(int segment_num)
{
    char *name = (char *)malloc(MAX_PATH);
    if (name != NULL)
    {
        sprintf(name, DATA_SEGMENT_NAME_PATTERN, segment_num);
    }
    return name;
}

/*
 * Translate a block number to a segment address
 */
block_addr_t translate_block_address(uint32_t block_num)
{
    block_addr_t addr;
    addr.segment_num = block_num / BLOCKS_PER_DATA_SEGMENT;
    addr.block_num = block_num % BLOCKS_PER_DATA_SEGMENT;
    return addr;
}

/*
 * Bitmap operations
 */

void set_bit(uint8_t *bitmap, int bit_num)
{
    bitmap[bit_num / 8] |= (1 << (bit_num % 8));
}

void clear_bit(uint8_t *bitmap, int bit_num)
{
    bitmap[bit_num / 8] &= ~(1 << (bit_num % 8));
}

int test_bit(const uint8_t *bitmap, int bit_num)
{
    return (bitmap[bit_num / 8] & (1 << (bit_num % 8))) != 0;
}

int find_first_zero_bit(const uint8_t *bitmap, int size)
{
    for (int i = 0; i < size; i++)
    {
        if (!test_bit(bitmap, i))
        {
            return i;
        }
    }
    return -1; // No free bits
}

/*
 * Directory operations
 */

/**
 * Add a directory entry to a directory
 */
int add_directory_entry(uint32_t dir_inode, const char *name, uint32_t entry_inode)
{
    inode_t inode;
    dir_entry_t new_entry;
    uint8_t block_buffer[BLOCK_SIZE];
    int entry_added = 0;
    uint32_t block_num;
    int i, j;

    // 1. Read the directory inode
    if (read_inode(dir_inode, &inode) != 0)
    {
        fprintf(stderr, "Failed to read directory inode %u\n", dir_inode);
        return -1;
    }

    // Check if this is actually a directory
    if (inode.type != FILE_TYPE_DIRECTORY)
    {
        fprintf(stderr, "Inode %u is not a directory\n", dir_inode);
        return -1;
    }

    // Initialize the new entry
    strncpy(new_entry.name, name, MAX_FILENAME - 1);
    new_entry.name[MAX_FILENAME - 1] = '\0'; // Ensure null termination
    new_entry.inode_number = entry_inode;

    // 2 & 3. Read data blocks and find space for new entry
    // First, try to find space in existing blocks
    for (i = 0; i < MAX_DIRECT_BLOCKS && inode.direct_blocks[i] != BLOCK_NULL; i++)
    {
        block_num = inode.direct_blocks[i];

        if (read_block(block_num, block_buffer) != 0)
        {
            fprintf(stderr, "Failed to read directory block %u\n", block_num);
            continue;
        }

        // Each block can hold BLOCK_SIZE / sizeof(dir_entry_t) entries
        int entries_per_block = BLOCK_SIZE / sizeof(dir_entry_t);
        dir_entry_t *entries = (dir_entry_t *)block_buffer;

        // Look for an unused entry (inode_number == 0) or the end of entries
        for (j = 0; j < entries_per_block; j++)
        {
            if (entries[j].inode_number == 0)
            {
                // Found an unused slot
                entries[j] = new_entry;
                entry_added = 1;

                // Write the updated block back
                if (write_block(block_num, block_buffer) != 0)
                {
                    fprintf(stderr, "Failed to write directory block %u\n", block_num);
                    return -1;
                }

                // ── Newly added: bump the directory's size and rewrite its inode ──
                inode.size += sizeof(dir_entry_t);
                if (write_inode(dir_inode, &inode) != 0)
                {
                    fprintf(stderr,
                            "Failed to update parent directory inode metadata\n");
                    return -1;
                }

                break;
            }

            // Check if entry with this name already exists
            if (strcmp(entries[j].name, name) == 0)
            {
                fprintf(stderr, "Entry with name %s already exists\n", name);
                return -1;
            }
        }

        if (entry_added)
        {
            break;
        }
    }

    // If no space found in existing blocks, allocate a new block
    if (!entry_added)
    {
        // Find next available direct block slot
        for (i = 0; i < MAX_DIRECT_BLOCKS; i++)
        {
            if (inode.direct_blocks[i] == BLOCK_NULL)
            {
                break;
            }
        }

        if (i >= MAX_DIRECT_BLOCKS)
        {
            fprintf(stderr, "Directory is full (no more direct blocks available)\n");
            return -1;
        }

        // Allocate a new block
        block_num = allocate_block();
        if (block_num == BLOCK_NULL)
        {
            fprintf(stderr, "Failed to allocate new directory block\n");
            return -1;
        }

        // Initialize the block with zeroes
        memset(block_buffer, 0, BLOCK_SIZE);

        // Add our new entry as the first entry
        dir_entry_t *entries = (dir_entry_t *)block_buffer;
        entries[0] = new_entry;

        // Write the new block
        if (write_block(block_num, block_buffer) != 0)
        {
            fprintf(stderr, "Failed to write new directory block %u\n", block_num);
            free_block(block_num);
            return -1;
        }

        // Update the inode to point to this new block
        inode.direct_blocks[i] = block_num;
        inode.size += sizeof(dir_entry_t);

        // Write the updated inode back to disk
        if (write_inode(dir_inode, &inode) != 0)
        {
            fprintf(stderr, "Failed to update directory inode %u\n", dir_inode);
            return -1;
        }
    }

    return 0;
}

/**
 * Remove a directory entry from a directory
 */
int remove_directory_entry(uint32_t dir_inode, const char *name)
{
    inode_t inode;
    uint8_t block_buffer[BLOCK_SIZE];
    int entry_removed = 0;
    uint32_t block_num;
    int i, j;

    // 1. Read the directory inode
    if (read_inode(dir_inode, &inode) != 0)
    {
        fprintf(stderr, "Failed to read directory inode %u\n", dir_inode);
        return -1;
    }

    // Check if this is actually a directory
    if (inode.type != FILE_TYPE_DIRECTORY)
    {
        fprintf(stderr, "Inode %u is not a directory\n", dir_inode);
        return -1;
    }

    // 2 & 3. Read data blocks and find the entry to remove
    for (i = 0; i < MAX_DIRECT_BLOCKS && inode.direct_blocks[i] != BLOCK_NULL; i++)
    {
        block_num = inode.direct_blocks[i];

        if (read_block(block_num, block_buffer) != 0)
        {
            fprintf(stderr, "Failed to read directory block %u\n", block_num);
            continue;
        }

        // Parse the entries in this block
        int entries_per_block = BLOCK_SIZE / sizeof(dir_entry_t);
        dir_entry_t *entries = (dir_entry_t *)block_buffer;

        // Look for the entry with the matching name
        for (j = 0; j < entries_per_block; j++)
        {
            if (entries[j].inode_number != 0 && strcmp(entries[j].name, name) == 0)
            {
                // Found the entry to remove

                // Mark the entry as unused by setting inode_number to 0
                entries[j].inode_number = 0;
                entry_removed = 1;

                // Write the updated block back
                if (write_block(block_num, block_buffer) != 0)
                {
                    fprintf(stderr, "Failed to write directory block %u\n", block_num);
                    return -1;
                }

                break;
            }
        }

        if (entry_removed)
        {
            break;
        }
    }

    if (!entry_removed)
    {
        fprintf(stderr, "Entry with name %s not found\n", name);
        return -1;
    }

    // Check if the last block is now empty and can be freed
    // This is an optional optimization
    if (i == MAX_DIRECT_BLOCKS - 1 || inode.direct_blocks[i + 1] == BLOCK_NULL)
    {
        // This is the last used block, check if it's empty
        int entries_per_block = BLOCK_SIZE / sizeof(dir_entry_t);
        dir_entry_t *entries = (dir_entry_t *)block_buffer;
        int empty = 1;

        for (j = 0; j < entries_per_block; j++)
        {
            if (entries[j].inode_number != 0)
            {
                empty = 0;
                break;
            }
        }

        if (empty)
        {
            // Free the block
            free_block(block_num);
            inode.direct_blocks[i] = BLOCK_NULL;

            // Update the directory size
            if (inode.size >= BLOCK_SIZE)
            {
                inode.size -= BLOCK_SIZE;
            }
            else
            {
                inode.size = 0;
            }

            // Write the updated inode back to disk
            if (write_inode(dir_inode, &inode) != 0)
            {
                fprintf(stderr, "Failed to update directory inode %u\n", dir_inode);
                return -1;
            }
        }
    }

    return 0;
}

/**
 * Read all entries from a directory
 */
int read_directory_entries(uint32_t dir_inode, dir_entry_t **entries, int *count)
{
    inode_t inode;
    uint8_t block_buffer[BLOCK_SIZE];
    int total_entries = 0;
    int max_entries = 0;
    dir_entry_t *result = NULL;
    int i, j;

    // 1. Read the directory inode
    if (read_inode(dir_inode, &inode) != 0)
    {
        fprintf(stderr, "Failed to read directory inode %u\n", dir_inode);
        return -1;
    }

    // Check if this is actually a directory
    if (inode.type != FILE_TYPE_DIRECTORY)
    {
        fprintf(stderr, "Inode %u is not a directory\n", dir_inode);
        return -1;
    }

    // Calculate maximum number of entries (rough estimate)
    int entries_per_block = BLOCK_SIZE / sizeof(dir_entry_t);
    for (i = 0; i < MAX_DIRECT_BLOCKS && inode.direct_blocks[i] != BLOCK_NULL; i++)
    {
        max_entries += entries_per_block;
    }

    // Allocate memory for the maximum possible entries
    result = (dir_entry_t *)malloc(max_entries * sizeof(dir_entry_t));
    if (!result)
    {
        fprintf(stderr, "Failed to allocate memory for directory entries\n");
        return -1;
    }

    // 2 & 3. Read data blocks and parse the entries
    for (i = 0; i < MAX_DIRECT_BLOCKS && inode.direct_blocks[i] != BLOCK_NULL; i++)
    {
        if (read_block(inode.direct_blocks[i], block_buffer) != 0)
        {
            fprintf(stderr, "Failed to read directory block %u\n", inode.direct_blocks[i]);
            continue;
        }

        // Parse the entries in this block
        dir_entry_t *block_entries = (dir_entry_t *)block_buffer;

        for (j = 0; j < entries_per_block; j++)
        {
            if (block_entries[j].inode_number != 0)
            {
                // This is a valid entry, add it to our result
                result[total_entries++] = block_entries[j];
            }
        }
    }

    // 4. Return the array of entries
    *entries = result;
    *count = total_entries;

    return 0;
}

/*
 * Helper functions
 */

char *get_basename(const char *path)
{
    const char *last_slash = strrchr(path, '/');
    if (last_slash == NULL)
    {
        return strdup(path);
    }
    else if (*(last_slash + 1) == '\0')
    {
        // Path ends with slash, so get the previous component
        char *tmp = strndup(path, last_slash - path);
        char *basename = get_basename(tmp);
        free(tmp);
        return basename;
    }
    else
    {
        return strdup(last_slash + 1);
    }
}

char *get_parent_path(const char *path)
{
    const char *last_slash = strrchr(path, '/');
    if (last_slash == NULL)
    {
        return strdup(".");
    }
    else if (last_slash == path)
    {
        return strdup("/");
    }
    else
    {
        return strndup(path, last_slash - path);
    }
}

void ensure_path_exists(const char *path)
{
    char *parent_path;
    uint32_t parent_inode, current_inode;
    char *path_copy, *p, *component;

    // Handle root directory case
    if (strcmp(path, "/") == 0)
    {
        return; // Root always exists
    }

    // Make a copy of the path that we can modify
    path_copy = strdup(path);
    if (!path_copy)
    {
        fprintf(stderr, "Failed to allocate memory for path\n");
        return;
    }

    // Get the parent path
    parent_path = get_parent_path(path);
    if (!parent_path)
    {
        free(path_copy);
        fprintf(stderr, "Failed to get parent path\n");
        return;
    }

    // Make sure the parent path exists
    ensure_path_exists(parent_path);

    // Get the parent inode
    parent_inode = lookup_path(parent_path);
    if (parent_inode == 0)
    {
        free(path_copy);
        free(parent_path);
        fprintf(stderr, "Failed to locate parent directory\n");
        return;
    }

    // Get the basename
    char *basename = get_basename(path);
    if (!basename)
    {
        free(path_copy);
        free(parent_path);
        fprintf(stderr, "Failed to get basename\n");
        return;
    }

    // Check if this component already exists
    current_inode = lookup_path_component(parent_inode, basename);
    if (current_inode == 0)
    {
        // Create the directory
        create_directory(parent_inode, basename);
    }

    free(basename);
    free(parent_path);
    free(path_copy);
}

/*
 * Look up a single path component within a directory
 */
uint32_t lookup_path_component(uint32_t dir_inode, const char *name)
{
    dir_entry_t *entries = NULL;
    int count = 0;
    uint32_t found_inode = 0;
    int i;

    // Read all entries in this directory
    if (read_directory_entries(dir_inode, &entries, &count) != 0)
    {
        fprintf(stderr, "Failed to read directory entries\n");
        return 0;
    }

    // Look for an entry with the matching name
    for (i = 0; i < count; i++)
    {
        if (strcmp(entries[i].name, name) == 0)
        {
            found_inode = entries[i].inode_number;
            break;
        }
    }

    // Free the entries array
    free(entries);

    return found_inode;
}

/**
 * Create a new directory under the specified parent
 */
int create_directory(uint32_t parent_inode, const char *name)
{
    uint32_t new_inode, block_num;
    inode_t inode;
    uint8_t block_buffer[BLOCK_SIZE];

    // Allocate a new inode for the directory
    new_inode = allocate_inode();
    if (new_inode == 0)
    {
        fprintf(stderr, "Failed to allocate inode for new directory\n");
        return -1;
    }

    // Initialize the inode
    memset(&inode, 0, sizeof(inode_t));
    inode.type = FILE_TYPE_DIRECTORY;
    inode.size = 0;

    // Allocate a block for directory entries
    block_num = allocate_block();
    if (block_num == BLOCK_NULL)
    {
        fprintf(stderr, "Failed to allocate block for new directory\n");
        free_inode(new_inode);
        return -1;
    }

    // Initialize the block
    memset(block_buffer, 0, BLOCK_SIZE);

    // Set up "." and ".." entries
    dir_entry_t *entries = (dir_entry_t *)block_buffer;

    // "." entry points to this directory
    strncpy(entries[0].name, ".", MAX_FILENAME - 1);
    entries[0].name[MAX_FILENAME - 1] = '\0';
    entries[0].inode_number = new_inode;

    // ".." entry points to parent directory
    strncpy(entries[1].name, "..", MAX_FILENAME - 1);
    entries[1].name[MAX_FILENAME - 1] = '\0';
    entries[1].inode_number = parent_inode;

    // Write the block
    if (write_block(block_num, block_buffer) != 0)
    {
        fprintf(stderr, "Failed to write directory entries block\n");
        free_block(block_num);
        free_inode(new_inode);
        return -1;
    }

    // Update the inode to point to this block
    inode.direct_blocks[0] = block_num;
    inode.size = 2 * sizeof(dir_entry_t); // Two entries: "." and ".."

    // Write the inode
    if (write_inode(new_inode, &inode) != 0)
    {
        fprintf(stderr, "Failed to write directory inode\n");
        free_block(block_num);
        free_inode(new_inode);
        return -1;
    }

    // Add an entry for this directory in the parent
    if (add_directory_entry(parent_inode, name, new_inode) != 0)
    {
        fprintf(stderr, "Failed to add directory entry in parent\n");
        free_block(block_num);
        free_inode(new_inode);
        return -1;
    }

    return 0;
}