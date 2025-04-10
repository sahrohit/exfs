#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

#define SEGMENT_SIZE (1 * 1024 * 1024)  // 1MB
#define BLOCK_SIZE 4096                 // 4KB
#define MAX_FILENAME 255
#define MAX_PATH 1024
#define BLOCKS_PER_SEGMENT ((SEGMENT_SIZE - sizeof(BlockBitmap)) / BLOCK_SIZE)
#define INODES_PER_SEGMENT ((SEGMENT_SIZE - sizeof(InodeBitmap)) / sizeof(Inode))

// Segment types
typedef enum {
    INODE_SEGMENT,
    DATA_SEGMENT
} SegmentType;

// File types
typedef enum {
    REGULAR_FILE,
    DIRECTORY
} FileType;

// Directory entry structure
typedef struct {
    char name[MAX_FILENAME];
    uint32_t inode_number;
} DirEntry;

// Bitmap structures for tracking free inodes and blocks
typedef struct {
    uint8_t bits[SEGMENT_SIZE / 8 / BLOCK_SIZE]; // Bitmap for blocks
} BlockBitmap;

typedef struct {
    uint8_t bits[SEGMENT_SIZE / 8 / sizeof(struct Inode)]; // Bitmap for inodes
} InodeBitmap;

// Forward declaration for Inode
typedef struct Inode Inode;

// Calculate how many direct blocks can fit
#define NUM_DIRECT_BLOCKS ((BLOCK_SIZE - 4 * sizeof(uint32_t) - sizeof(FileType) - sizeof(uint64_t)) / sizeof(uint32_t))

// Inode structure
struct Inode {
    FileType type;                      // Regular file or directory
    uint64_t size;                      // Size of file in bytes
    uint32_t direct_blocks[NUM_DIRECT_BLOCKS]; // Direct block pointers
    uint32_t single_indirect;           // Single indirect block pointer
    uint32_t double_indirect;           // Double indirect block pointer
    uint32_t triple_indirect;           // Triple indirect block pointer
};

// Global variables
char inode_segment_prefix[] = "inode_segment_";
char data_segment_prefix[] = "data_segment_";
uint32_t current_inode_segment = 0;
uint32_t current_data_segment = 0;

// Function prototypes
void init_file_system();
uint32_t allocate_inode();
uint32_t allocate_block();
void write_inode(uint32_t inode_num, Inode *inode);
void read_inode(uint32_t inode_num, Inode *inode);
void write_block(uint32_t block_num, void *data);
void read_block(uint32_t block_num, void *data);
void list_contents(uint32_t inode_num, int depth);
void add_file(const char *fs_path, const char *local_path);
void remove_file(const char *fs_path);
void extract_file(const char *fs_path);
void debug_path(const char *fs_path);
uint32_t find_inode_by_path(const char *path);
uint32_t create_directory(uint32_t parent_inode, const char *name);
void add_dir_entry(uint32_t dir_inode, const char *name, uint32_t inode_num);
uint32_t find_dir_entry(uint32_t dir_inode, const char *name);
void remove_dir_entry(uint32_t dir_inode, const char *name);
char *get_inode_segment_name(uint32_t segment_num);
char *get_data_segment_name(uint32_t segment_num);

// Utility functions
int get_bit(uint8_t *bitmap, int bit_num) {
    return (bitmap[bit_num / 8] >> (bit_num % 8)) & 1;
}

void set_bit(uint8_t *bitmap, int bit_num) {
    bitmap[bit_num / 8] |= (1 << (bit_num % 8));
}

void clear_bit(uint8_t *bitmap, int bit_num) {
    bitmap[bit_num / 8] &= ~(1 << (bit_num % 8));
}

// Get the name of an inode segment file
char *get_inode_segment_name(uint32_t segment_num) {
    static char name[MAX_FILENAME];
    snprintf(name, MAX_FILENAME, "%s%u", inode_segment_prefix, segment_num);
    return name;
}

// Get the name of a data segment file
char *get_data_segment_name(uint32_t segment_num) {
    static char name[MAX_FILENAME];
    snprintf(name, MAX_FILENAME, "%s%u", data_segment_prefix, segment_num);
    return name;
}

// Initialize the file system (create initial segments if needed)
void init_file_system() {
    FILE *fp;
    
    // Check if inode segment 0 exists
    fp = fopen(get_inode_segment_name(0), "rb");
    if (fp == NULL) {
        // Create the first inode segment
        fp = fopen(get_inode_segment_name(0), "wb+");
        if (fp == NULL) {
            perror("Failed to create initial inode segment");
            exit(EXIT_FAILURE);
        }
        
        // Initialize segment with zeros
        char *buffer = calloc(1, SEGMENT_SIZE);
        if (buffer == NULL) {
            perror("Memory allocation failed");
            exit(EXIT_FAILURE);
        }
        
        // Mark inode 0 as used (for root directory)
        InodeBitmap *inode_bitmap = (InodeBitmap *)buffer;
        set_bit(inode_bitmap->bits, 0);
        
        // Initialize root directory inode
        Inode *root_inode = (Inode *)(buffer + sizeof(InodeBitmap));
        root_inode->type = DIRECTORY;
        root_inode->size = 0;
        memset(root_inode->direct_blocks, 0, sizeof(root_inode->direct_blocks));
        root_inode->single_indirect = 0;
        root_inode->double_indirect = 0;
        root_inode->triple_indirect = 0;
        
        // Write the initialized segment
        fwrite(buffer, SEGMENT_SIZE, 1, fp);
        free(buffer);
        fclose(fp);
        
        // Now create the first data segment
        fp = fopen(get_data_segment_name(0), "wb+");
        if (fp == NULL) {
            perror("Failed to create initial data segment");
            exit(EXIT_FAILURE);
        }
        
        // Initialize segment with zeros
        buffer = calloc(1, SEGMENT_SIZE);
        if (buffer == NULL) {
            perror("Memory allocation failed");
            exit(EXIT_FAILURE);
        }
        
        // Write the initialized segment
        fwrite(buffer, SEGMENT_SIZE, 1, fp);
        free(buffer);
        fclose(fp);
        
        current_inode_segment = 0;
        current_data_segment = 0;
    } else {
        fclose(fp);
        
        // Find the highest existing segment numbers
        uint32_t segment_num = 0;
        while (1) {
            fp = fopen(get_inode_segment_name(segment_num + 1), "rb");
            if (fp == NULL) {
                current_inode_segment = segment_num;
                break;
            }
            fclose(fp);
            segment_num++;
        }
        
        segment_num = 0;
        while (1) {
            fp = fopen(get_data_segment_name(segment_num + 1), "rb");
            if (fp == NULL) {
                current_data_segment = segment_num;
                break;
            }
            fclose(fp);
            segment_num++;
        }
    }
}

// Allocate a new inode and return its number
uint32_t allocate_inode() {
    FILE *fp;
    InodeBitmap inode_bitmap;
    
    // Search existing inode segments for a free inode
    for (uint32_t segment_num = 0; segment_num <= current_inode_segment; segment_num++) {
        fp = fopen(get_inode_segment_name(segment_num), "r+b");
        if (fp == NULL) {
            perror("Error opening inode segment");
            exit(EXIT_FAILURE);
        }
        
        // Read the inode bitmap
        if (fread(&inode_bitmap, sizeof(InodeBitmap), 1, fp) != 1) {
            perror("Error reading inode bitmap");
            fclose(fp);
            exit(EXIT_FAILURE);
        }
        
        // Find a free inode
        for (uint32_t i = 0; i < INODES_PER_SEGMENT; i++) {
            if (!get_bit(inode_bitmap.bits, i)) {
                // Mark this inode as allocated
                set_bit(inode_bitmap.bits, i);
                
                // Write the updated bitmap back
                fseek(fp, 0, SEEK_SET);
                if (fwrite(&inode_bitmap, sizeof(InodeBitmap), 1, fp) != 1) {
                    perror("Error writing inode bitmap");
                    fclose(fp);
                    exit(EXIT_FAILURE);
                }
                
                fclose(fp);
                return segment_num * INODES_PER_SEGMENT + i;
            }
        }
        
        fclose(fp);
    }
    
    // If we get here, we need to create a new inode segment
    current_inode_segment++;
    fp = fopen(get_inode_segment_name(current_inode_segment), "wb+");
    if (fp == NULL) {
        perror("Failed to create new inode segment");
        exit(EXIT_FAILURE);
    }
    
    // Initialize segment with zeros
    char *buffer = calloc(1, SEGMENT_SIZE);
    if (buffer == NULL) {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    
    // Mark the first inode as used
    InodeBitmap *bitmap = (InodeBitmap *)buffer;
    set_bit(bitmap->bits, 0);
    
    // Write the initialized segment
    fwrite(buffer, SEGMENT_SIZE, 1, fp);
    free(buffer);
    fclose(fp);
    
    // Return the first inode in the new segment
    return current_inode_segment * INODES_PER_SEGMENT + 0;
}

// Allocate a new data block and return its number
uint32_t allocate_block() {
    FILE *fp;
    BlockBitmap block_bitmap;
    
    // Search existing data segments for a free block
    for (uint32_t segment_num = 0; segment_num <= current_data_segment; segment_num++) {
        fp = fopen(get_data_segment_name(segment_num), "r+b");
        if (fp == NULL) {
            perror("Error opening data segment");
            exit(EXIT_FAILURE);
        }
        
        // Read the block bitmap
        if (fread(&block_bitmap, sizeof(BlockBitmap), 1, fp) != 1) {
            perror("Error reading block bitmap");
            fclose(fp);
            exit(EXIT_FAILURE);
        }
        
        // Find a free block
        for (uint32_t i = 0; i < BLOCKS_PER_SEGMENT; i++) {
            if (!get_bit(block_bitmap.bits, i)) {
                // Mark this block as allocated
                set_bit(block_bitmap.bits, i);
                
                // Write the updated bitmap back
                fseek(fp, 0, SEEK_SET);
                if (fwrite(&block_bitmap, sizeof(BlockBitmap), 1, fp) != 1) {
                    perror("Error writing block bitmap");
                    fclose(fp);
                    exit(EXIT_FAILURE);
                }
                
                fclose(fp);
                return segment_num * BLOCKS_PER_SEGMENT + i;
            }
        }
        
        fclose(fp);
    }
    
    // If we get here, we need to create a new data segment
    current_data_segment++;
    fp = fopen(get_data_segment_name(current_data_segment), "wb+");
    if (fp == NULL) {
        perror("Failed to create new data segment");
        exit(EXIT_FAILURE);
    }
    
    // Initialize segment with zeros
    char *buffer = calloc(1, SEGMENT_SIZE);
    if (buffer == NULL) {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    
    // Mark the first block as used
    BlockBitmap *bitmap = (BlockBitmap *)buffer;
    set_bit(bitmap->bits, 0);
    
    // Write the initialized segment
    fwrite(buffer, SEGMENT_SIZE, 1, fp);
    free(buffer);
    fclose(fp);
    
    // Return the first block in the new segment
    return current_data_segment * BLOCKS_PER_SEGMENT + 0;
}

// Write an inode to storage
void write_inode(uint32_t inode_num, Inode *inode) {
    uint32_t segment_num = inode_num / INODES_PER_SEGMENT;
    uint32_t offset = inode_num % INODES_PER_SEGMENT;
    
    FILE *fp = fopen(get_inode_segment_name(segment_num), "r+b");
    if (fp == NULL) {
        perror("Error opening inode segment for writing");
        exit(EXIT_FAILURE);
    }
    
    // Seek to the position of this inode
    fseek(fp, sizeof(InodeBitmap) + offset * sizeof(Inode), SEEK_SET);
    
    // Write the inode
    if (fwrite(inode, sizeof(Inode), 1, fp) != 1) {
        perror("Error writing inode");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    
    fclose(fp);
}

// Read an inode from storage
void read_inode(uint32_t inode_num, Inode *inode) {
    uint32_t segment_num = inode_num / INODES_PER_SEGMENT;
    uint32_t offset = inode_num % INODES_PER_SEGMENT;
    
    FILE *fp = fopen(get_inode_segment_name(segment_num), "rb");
    if (fp == NULL) {
        perror("Error opening inode segment for reading");
        exit(EXIT_FAILURE);
    }
    
    // Seek to the position of this inode
    fseek(fp, sizeof(InodeBitmap) + offset * sizeof(Inode), SEEK_SET);
    
    // Read the inode
    if (fread(inode, sizeof(Inode), 1, fp) != 1) {
        perror("Error reading inode");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    
    fclose(fp);
}

// Write a data block
void write_block(uint32_t block_num, void *data) {
    uint32_t segment_num = block_num / BLOCKS_PER_SEGMENT;
    uint32_t offset = block_num % BLOCKS_PER_SEGMENT;
    
    FILE *fp = fopen(get_data_segment_name(segment_num), "r+b");
    if (fp == NULL) {
        perror("Error opening data segment for writing");
        exit(EXIT_FAILURE);
    }
    
    // Seek to the position of this block
    fseek(fp, sizeof(BlockBitmap) + offset * BLOCK_SIZE, SEEK_SET);
    
    // Write the block
    if (fwrite(data, BLOCK_SIZE, 1, fp) != 1) {
        perror("Error writing block");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    
    fclose(fp);
}

// Read a data block
void read_block(uint32_t block_num, void *data) {
    uint32_t segment_num = block_num / BLOCKS_PER_SEGMENT;
    uint32_t offset = block_num % BLOCKS_PER_SEGMENT;
    
    FILE *fp = fopen(get_data_segment_name(segment_num), "rb");
    if (fp == NULL) {
        perror("Error opening data segment for reading");
        exit(EXIT_FAILURE);
    }
    
    // Seek to the position of this block
    fseek(fp, sizeof(BlockBitmap) + offset * BLOCK_SIZE, SEEK_SET);
    
    // Read the block
    if (fread(data, BLOCK_SIZE, 1, fp) != 1) {
        perror("Error reading block");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    
    fclose(fp);
}

// Free a block
void free_block(uint32_t block_num) {
    uint32_t segment_num = block_num / BLOCKS_PER_SEGMENT;
    uint32_t offset = block_num % BLOCKS_PER_SEGMENT;
    
    FILE *fp = fopen(get_data_segment_name(segment_num), "r+b");
    if (fp == NULL) {
        perror("Error opening data segment");
        exit(EXIT_FAILURE);
    }
    
    // Read the block bitmap
    BlockBitmap block_bitmap;
    if (fread(&block_bitmap, sizeof(BlockBitmap), 1, fp) != 1) {
        perror("Error reading block bitmap");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    
    // Mark the block as free
    clear_bit(block_bitmap.bits, offset);
    
    // Write the updated bitmap back
    fseek(fp, 0, SEEK_SET);
    if (fwrite(&block_bitmap, sizeof(BlockBitmap), 1, fp) != 1) {
        perror("Error writing block bitmap");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    
    fclose(fp);
}

// Free an inode
void free_inode(uint32_t inode_num) {
    uint32_t segment_num = inode_num / INODES_PER_SEGMENT;
    uint32_t offset = inode_num % INODES_PER_SEGMENT;
    
    FILE *fp = fopen(get_inode_segment_name(segment_num), "r+b");
    if (fp == NULL) {
        perror("Error opening inode segment");
        exit(EXIT_FAILURE);
    }
    
    // Read the inode bitmap
    InodeBitmap inode_bitmap;
    if (fread(&inode_bitmap, sizeof(InodeBitmap), 1, fp) != 1) {
        perror("Error reading inode bitmap");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    
    // Mark the inode as free
    clear_bit(inode_bitmap.bits, offset);
    
    // Write the updated bitmap back
    fseek(fp, 0, SEEK_SET);
    if (fwrite(&inode_bitmap, sizeof(InodeBitmap), 1, fp) != 1) {
        perror("Error writing inode bitmap");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    
    fclose(fp);
}

// Find an inode by path
uint32_t find_inode_by_path(const char *path) {
    char path_copy[MAX_PATH];
    strncpy(path_copy, path, MAX_PATH);
    path_copy[MAX_PATH - 1] = '\0';
    
    // Start from the root
    uint32_t current_inode = 0;
    
    // Handle the root directory case
    if (strcmp(path, "/") == 0) {
        return current_inode;
    }
    
    // Skip leading slash
    char *token = strtok(path_copy + 1, "/");
    while (token != NULL) {
        Inode inode;
        read_inode(current_inode, &inode);
        
        if (inode.type != DIRECTORY) {
            fprintf(stderr, "Error: %s is not a directory\n", token);
            return (uint32_t)-1;
        }
        
        // Search this directory for the token
        uint32_t found_inode = find_dir_entry(current_inode, token);
        if (found_inode == (uint32_t)-1) {
            // Entry not found
            return (uint32_t)-1;
        }
        
        current_inode = found_inode;
        token = strtok(NULL, "/");
    }
    
    return current_inode;
}

// Create a directory
uint32_t create_directory(uint32_t parent_inode, const char *name) {
    // Allocate a new inode for the directory
    uint32_t new_inode = allocate_inode();
    
    // Initialize the directory inode
    Inode dir_inode;
    dir_inode.type = DIRECTORY;
    dir_inode.size = 0;
    memset(dir_inode.direct_blocks, 0, sizeof(dir_inode.direct_blocks));
    dir_inode.single_indirect = 0;
    dir_inode.double_indirect = 0;
    dir_inode.triple_indirect = 0;
    
    // Write the directory inode
    write_inode(new_inode, &dir_inode);
    
    // Add an entry for this directory to the parent
    add_dir_entry(parent_inode, name, new_inode);
    
    return new_inode;
}

// Add a directory entry
void add_dir_entry(uint32_t dir_inode, const char *name, uint32_t inode_num) {
    Inode inode;
    read_inode(dir_inode, &inode);
    
    if (inode.type != DIRECTORY) {
        fprintf(stderr, "Error: Not a directory\n");
        exit(EXIT_FAILURE);
    }
    
    // Calculate how many entries fit in a block
    int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);
    
    // Calculate the number of entries we currently have
    int num_entries = inode.size / sizeof(DirEntry);
    
    // Calculate which block and offset this entry will go in
    int block_index = num_entries / entries_per_block;
    int entry_offset = num_entries % entries_per_block;
    
    // Create a new entry
    DirEntry new_entry;
    strncpy(new_entry.name, name, MAX_FILENAME);
    new_entry.name[MAX_FILENAME - 1] = '\0';
    new_entry.inode_number = inode_num;
    
    // Check if we need to allocate a new block
    if (block_index >= NUM_DIRECT_BLOCKS) {
        // Would need to implement indirect blocks here
        fprintf(stderr, "Error: Directory too large (indirect blocks not implemented)\n");
        exit(EXIT_FAILURE);
    }
    
    if (entry_offset == 0) {
        // Need to allocate a new block
        uint32_t new_block = allocate_block();
        inode.direct_blocks[block_index] = new_block;
        
        // Initialize the block with zeros
        char block_data[BLOCK_SIZE] = {0};
        
        // Add our entry
        memcpy(block_data, &new_entry, sizeof(DirEntry));
        
        // Write the block
        write_block(new_block, block_data);
    } else {
        // Read existing block
        char block_data[BLOCK_SIZE];
        read_block(inode.direct_blocks[block_index], block_data);
        
        // Add our entry
        memcpy(block_data + entry_offset * sizeof(DirEntry), &new_entry, sizeof(DirEntry));
        
        // Write the block back
        write_block(inode.direct_blocks[block_index], block_data);
    }
    
    // Update the inode size
    inode.size += sizeof(DirEntry);
    write_inode(dir_inode, &inode);
}

// Find a directory entry
uint32_t find_dir_entry(uint32_t dir_inode, const char *name) {
    Inode inode;
    read_inode(dir_inode, &inode);
    
    if (inode.type != DIRECTORY) {
        fprintf(stderr, "Error: Not a directory\n");
        return (uint32_t)-1;
    }
    
    // Calculate how many entries fit in a block
    int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);
    
    // Calculate the number of entries we currently have
    int num_entries = inode.size / sizeof(DirEntry);
    
    // Iterate through all blocks and entries
    for (int block_index = 0; block_index < NUM_DIRECT_BLOCKS && block_index * entries_per_block < num_entries; block_index++) {
        if (inode.direct_blocks[block_index] == 0) {
            continue;
        }
        
        // Read this block
        char block_data[BLOCK_SIZE];
        read_block(inode.direct_blocks[block_index], block_data);
        
        // Check each entry in this block
        int entries_in_this_block = (num_entries - block_index * entries_per_block < entries_per_block) ? 
                                    (num_entries - block_index * entries_per_block) : entries_per_block;
        
        for (int i = 0; i < entries_in_this_block; i++) {
            DirEntry *entry = (DirEntry *)(block_data + i * sizeof(DirEntry));
            if (strcmp(entry->name, name) == 0) {
                return entry->inode_number;
            }
        }
    }
    
    // Not found
    return (uint32_t)-1;
}

// Remove a directory entry
void remove_dir_entry(uint32_t dir_inode, const char *name) {
    Inode inode;
    read_inode(dir_inode, &inode);
    
    if (inode.type != DIRECTORY) {
        fprintf(stderr, "Error: Not a directory\n");
        return;
    }
    
    // Calculate how many entries fit in a block
    int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);
    
    // Calculate the number of entries we currently have
    int num_entries = inode.size / sizeof(DirEntry);
    
    // Iterate through all blocks and entries
    for (int block_index = 0; block_index < NUM_DIRECT_BLOCKS && block_index * entries_per_block < num_entries; block_index++) {
        if (inode.direct_blocks[block_index] == 0) {
            continue;
        }
        
        // Read this block
        char block_data[BLOCK_SIZE];
        read_block(inode.direct_blocks[block_index], block_data);
        
        // Check each entry in this block
        int entries_in_this_block = (num_entries - block_index * entries_per_block < entries_per_block) ? 
                                    (num_entries - block_index * entries_per_block) : entries_per_block;
        
        for (int i = 0; i < entries_in_this_block; i++) {
            DirEntry *entry = (DirEntry *)(block_data + i * sizeof(DirEntry));
            if (strcmp(entry->name, name) == 0) {
                // Found it! Now remove it by moving the last entry to this spot
                DirEntry *last_entry = NULL;
                
                // Find the last entry
                int last_block_index = (num_entries - 1) / entries_per_block;
                int last_entry_offset = (num_entries - 1) % entries_per_block;
                
                if (last_block_index != block_index) {
                    // Last entry is in a different block
                    char last_block_data[BLOCK_SIZE];
                    read_block(inode.direct_blocks[last_block_index], last_block_data);
                    last_entry = (DirEntry *)(last_block_data + last_entry_offset * sizeof(DirEntry));
                    
                    // Copy the last entry to this spot
                    memcpy(entry, last_entry, sizeof(DirEntry));
                    
                    // Write this block back
                    write_block(inode.direct_blocks[block_index], block_data);
                    
                    // Zero out the last entry
                    memset(last_entry, 0, sizeof(DirEntry));
                    
                    // Write the last block back
                    write_block(inode.direct_blocks[last_block_index], last_block_data);
                } else {
                    // Last entry is in the same block
                    last_entry = (DirEntry *)(block_data + last_entry_offset * sizeof(DirEntry));
                    
                    // If it's not the same entry, copy the last entry to this spot
                    if (i != last_entry_offset) {
                        memcpy(entry, last_entry, sizeof(DirEntry));
                    }
                    
                    // Zero out the last entry
                    memset(last_entry, 0, sizeof(DirEntry));
                    
                    // Write this block back
                    write_block(inode.direct_blocks[block_index], block_data);
                }
                
                // Update the inode size
                inode.size -= sizeof(DirEntry);
                write_inode(dir_inode, &inode);
                
                return;
            }
        }
    }
    
    fprintf(stderr, "Warning: Entry '%s' not found in directory\n", name);
}

// // List the contents of the file system
// void list_contents(uint32_t inode_num, int depth) {
//     Inode inode;
//     read_inode(inode_num, &inode);
    
//     if (inode.type != DIRECTORY) {
//         return;
//     }
    
//     // Calculate how many entries fit in a block
//     int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);
    
//     // Calculate the number of entries we currently have
//     int num_entries = inode.size / sizeof(DirEntry);
    
//     // Iterate