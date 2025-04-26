#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SEGMENT_SIZE (1024 * 1024)        // 1MB segment size
#define BLOCK_SIZE 4096                   // 4KB block size
#define BITMAP_SIZE 255                   // Number of inodes/blocks per segment
#define MAX_DIRECT_BLOCKS 10              // Number of direct block pointers in inode
#define MAX_PATH_LENGTH 1024              // Maximum path length
#define MAX_FILENAME_LENGTH 255           // Maximum filename length
#define MAX_ENTRIES_PER_DIR 10            // Maximum number of entries per directory block

// File types
#define TYPE_FILE 1
#define TYPE_DIR 2

// Structure definitions
typedef struct {
    uint32_t type;                             // File type (regular or directory)
    uint64_t size;                             // File size in bytes
    uint32_t direct_blocks[MAX_DIRECT_BLOCKS]; // Direct block pointers
    uint32_t single_indirect;                  // Single indirect block
    uint32_t double_indirect;                  // Double indirect block
    uint32_t triple_indirect;                  // Triple indirect block
} inode_t;

typedef struct {
    char data[BLOCK_SIZE];
} datablock_t;

typedef struct {
    char name[MAX_FILENAME_LENGTH];
    uint32_t inode_num;
    uint32_t type;
    uint32_t inuse;
} dirent_t;

typedef struct {
    dirent_t entries[MAX_ENTRIES_PER_DIR];
} dirblock_t;

// Utility functions
int create_segment_if_not_exists(const char* segment_name) {
    FILE* file = fopen(segment_name, "rb+");
    if (file == NULL) {
        // Create the file
        file = fopen(segment_name, "wb+");
        if (file == NULL) {
            fprintf(stderr, "Error creating segment file %s\n", segment_name);
            return -1;
        }
        
        // Set file size to 1MB
        fseek(file, SEGMENT_SIZE - 1, SEEK_SET);
        fputc(0, file);
        
        // Initialize bitmap (all free)
        char bitmap[BLOCK_SIZE] = {0};
        fseek(file, 0, SEEK_SET);
        fwrite(bitmap, 1, BLOCK_SIZE, file);
        
        // If this is the first inode segment, mark the first inode as used (root directory)
        if (strcmp(segment_name, "inodeseg0") == 0) {
            fseek(file, 0, SEEK_SET);
            bitmap[0] = 1;  // Mark first inode as used
            fwrite(bitmap, 1, BLOCK_SIZE, file);
            
            // Initialize root directory inode
            inode_t root_inode;
            memset(&root_inode, 0, sizeof(inode_t));
            root_inode.type = TYPE_DIR;
            
            fseek(file, BLOCK_SIZE, SEEK_SET);  // Position after bitmap
            fwrite(&root_inode, sizeof(inode_t), 1, file);
        }
        
        fclose(file);
        return 0;
    }
    
    fclose(file);
    return 0;
}

int find_free_inode() {
    int inode_num = -1;
    int segment_num = 0;
    char segment_name[20];
    FILE* file = NULL;
    
    while (1) {
        sprintf(segment_name, "inodeseg%d", segment_num);
        file = fopen(segment_name, "rb+");
        
        if (file == NULL) {
            // Create a new segment
            if (create_segment_if_not_exists(segment_name) != 0) {
                return -1;
            }
            file = fopen(segment_name, "rb+");
            if (file == NULL) {
                return -1;
            }
        }
        
        // Read bitmap
        unsigned char bitmap[BLOCK_SIZE];
        fread(bitmap, 1, BLOCK_SIZE, file);
        
        // Find first free inode
        for (int i = 0; i < BITMAP_SIZE; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                inode_num = segment_num * BITMAP_SIZE + i;
                
                // Mark inode as used
                bitmap[i / 8] |= (1 << (i % 8));
                fseek(file, 0, SEEK_SET);
                fwrite(bitmap, 1, BLOCK_SIZE, file);
                
                fclose(file);
                return inode_num;
            }
        }
        
        fclose(file);
        segment_num++;
    }
    
    return -1;
}

int get_inode_segment_and_offset(int inode_num, int* segment_num, int* offset) {
    if (inode_num < 0) {
        return -1;
    }
    
    *segment_num = inode_num / BITMAP_SIZE;
    *offset = inode_num % BITMAP_SIZE;
    
    return 0;
}

int read_inode(int inode_num, inode_t* inode) {
    int segment_num, offset;
    if (get_inode_segment_and_offset(inode_num, &segment_num, &offset) != 0) {
        return -1;
    }
    
    char segment_name[20];
    sprintf(segment_name, "inodeseg%d", segment_num);
    
    FILE* file = fopen(segment_name, "rb");
    if (file == NULL) {
        return -1;
    }
    
    // Offset calculation: bitmap + inode position * inode size
    fseek(file, BLOCK_SIZE + offset * sizeof(inode_t), SEEK_SET);
    if (fread(inode, sizeof(inode_t), 1, file) != 1) {
        fclose(file);
        return -1;
    }
    
    fclose(file);
    return 0;
}

int write_inode(int inode_num, inode_t* inode) {
    int segment_num, offset;
    if (get_inode_segment_and_offset(inode_num, &segment_num, &offset) != 0) {
        return -1;
    }
    
    char segment_name[20];
    sprintf(segment_name, "inodeseg%d", segment_num);
    
    FILE* file = fopen(segment_name, "rb+");
    if (file == NULL) {
        return -1;
    }
    
    // Offset calculation: bitmap + inode position * inode size
    fseek(file, BLOCK_SIZE + offset * sizeof(inode_t), SEEK_SET);
    if (fwrite(inode, sizeof(inode_t), 1, file) != 1) {
        fclose(file);
        return -1;
    }
    
    fclose(file);
    return 0;
}

int find_free_data_block() {
    int block_num = -1;
    int segment_num = 0;
    char segment_name[20];
    FILE* file = NULL;
    
    while (1) {
        sprintf(segment_name, "dataseg%d", segment_num);
        file = fopen(segment_name, "rb+");
        
        if (file == NULL) {
            // Create a new segment
            if (create_segment_if_not_exists(segment_name) != 0) {
                return -1;
            }
            file = fopen(segment_name, "rb+");
            if (file == NULL) {
                return -1;
            }
        }
        
        // Read bitmap
        unsigned char bitmap[BLOCK_SIZE];
        fread(bitmap, 1, BLOCK_SIZE, file);
        
        // Find first free block
        for (int i = 0; i < BITMAP_SIZE; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                block_num = segment_num * BITMAP_SIZE + i;
                
                // Mark block as used
                bitmap[i / 8] |= (1 << (i % 8));
                fseek(file, 0, SEEK_SET);
                fwrite(bitmap, 1, BLOCK_SIZE, file);
                
                fclose(file);
                return block_num;
            }
        }
        
        fclose(file);
        segment_num++;
    }
    
    return -1;
}

int get_data_segment_and_offset(int block_num, int* segment_num, int* offset) {
    if (block_num < 0) {
        return -1;
    }
    
    *segment_num = block_num / BITMAP_SIZE;
    *offset = block_num % BITMAP_SIZE;
    
    return 0;
}

int read_data_block(int block_num, void* buffer) {
    int segment_num, offset;
    if (get_data_segment_and_offset(block_num, &segment_num, &offset) != 0) {
        return -1;
    }
    
    char segment_name[20];
    sprintf(segment_name, "dataseg%d", segment_num);
    
    FILE* file = fopen(segment_name, "rb");
    if (file == NULL) {
        return -1;
    }
    
    // Offset calculation: bitmap + block position * block size
    fseek(file, BLOCK_SIZE + offset * BLOCK_SIZE, SEEK_SET);
    if (fread(buffer, BLOCK_SIZE, 1, file) != 1) {
        fclose(file);
        return -1;
    }
    
    fclose(file);
    return 0;
}

int write_data_block(int block_num, void* buffer) {
    int segment_num, offset;
    if (get_data_segment_and_offset(block_num, &segment_num, &offset) != 0) {
        return -1;
    }
    
    char segment_name[20];
    sprintf(segment_name, "dataseg%d", segment_num);
    
    FILE* file = fopen(segment_name, "rb+");
    if (file == NULL) {
        return -1;
    }
    
    // Offset calculation: bitmap + block position * block size
    fseek(file, BLOCK_SIZE + offset * BLOCK_SIZE, SEEK_SET);
    if (fwrite(buffer, BLOCK_SIZE, 1, file) != 1) {
        fclose(file);
        return -1;
    }
    
    fclose(file);
    return 0;
}

// Initialize an empty directory data block
int init_directory_block(int block_num) {
    dirblock_t dir_block;
    memset(&dir_block, 0, sizeof(dirblock_t));
    
    return write_data_block(block_num, &dir_block);
}

// Find an entry in a directory by name
int find_dir_entry(int dir_inode_num, const char* name, dirent_t* entry) {
    inode_t dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0) {
        return -1;
    }
    
    if (dir_inode.type != TYPE_DIR) {
        return -1;  // Not a directory
    }
    
    // Check direct blocks
    for (int i = 0; i < MAX_DIRECT_BLOCKS && dir_inode.direct_blocks[i] != 0; i++) {
        dirblock_t dir_block;
        if (read_data_block(dir_inode.direct_blocks[i], &dir_block) != 0) {
            continue;
        }
        
        for (int j = 0; j < MAX_ENTRIES_PER_DIR; j++) {
            if (dir_block.entries[j].inuse && strcmp(dir_block.entries[j].name, name) == 0) {
                *entry = dir_block.entries[j];
                return 0;
            }
        }
    }
    
    // TODO: Search through indirect blocks if needed
    
    return -1;  // Entry not found
}

// Add an entry to a directory
int add_dir_entry(int dir_inode_num, const char* name, int entry_inode_num, int type) {
    inode_t dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0) {
        return -1;
    }
    
    if (dir_inode.type != TYPE_DIR) {
        return -1;  // Not a directory
    }
    
    // Find a free slot in existing blocks
    for (int i = 0; i < MAX_DIRECT_BLOCKS && dir_inode.direct_blocks[i] != 0; i++) {
        dirblock_t dir_block;
        if (read_data_block(dir_inode.direct_blocks[i], &dir_block) != 0) {
            continue;
        }
        
        for (int j = 0; j < MAX_ENTRIES_PER_DIR; j++) {
            if (!dir_block.entries[j].inuse) {
                // Found free slot
                memset(&dir_block.entries[j], 0, sizeof(dirent_t));
                strncpy(dir_block.entries[j].name, name, MAX_FILENAME_LENGTH - 1);
                dir_block.entries[j].inode_num = entry_inode_num;
                dir_block.entries[j].type = type;
                dir_block.entries[j].inuse = 1;
                
                if (write_data_block(dir_inode.direct_blocks[i], &dir_block) != 0) {
                    return -1;
                }
                
                return 0;
            }
        }
    }
    
    // Need to allocate a new block
    for (int i = 0; i < MAX_DIRECT_BLOCKS; i++) {
        if (dir_inode.direct_blocks[i] == 0) {
            // Allocate new block
            int new_block = find_free_data_block();
            if (new_block < 0) {
                return -1;
            }
            
            dir_inode.direct_blocks[i] = new_block;
            
            // Initialize directory block
            dirblock_t dir_block;
            memset(&dir_block, 0, sizeof(dirblock_t));
            
            // Add entry
            strncpy(dir_block.entries[0].name, name, MAX_FILENAME_LENGTH - 1);
            dir_block.entries[0].inode_num = entry_inode_num;
            dir_block.entries[0].type = type;
            dir_block.entries[0].inuse = 1;
            
            if (write_data_block(new_block, &dir_block) != 0) {
                return -1;
            }
            
            // Update directory inode
            if (write_inode(dir_inode_num, &dir_inode) != 0) {
                return -1;
            }
            
            return 0;
        }
    }
    
    // TODO: Handle indirect blocks if needed
    
    return -1;  // No space available
}

// Create a directory at the specified path
int create_directory(const char* path) {
    // Start at root directory
    int current_dir_inode = 0;  // Root directory is always inode 0
    
    // Make a copy of the path to tokenize
    char path_copy[MAX_PATH_LENGTH];
    strncpy(path_copy, path, MAX_PATH_LENGTH - 1);
    path_copy[MAX_PATH_LENGTH - 1] = '\0';
    
    // Remove trailing slash if present
    int len = strlen(path_copy);
    if (len > 1 && path_copy[len - 1] == '/') {
        path_copy[len - 1] = '\0';
    }
    
    // Root directory is already created
    if (strcmp(path_copy, "/") == 0) {
        return 0;
    }
    
    // Tokenize path
    char* saveptr;
    char* token = strtok_r(path_copy, "/", &saveptr);
    char* next_token = NULL;
    
    while (token != NULL) {
        next_token = strtok_r(NULL, "/", &saveptr);
        
        dirent_t entry;
        if (find_dir_entry(current_dir_inode, token, &entry) == 0) {
            // Entry exists
            if (entry.type != TYPE_DIR) {
                // Not a directory
                return -1;
            }
            current_dir_inode = entry.inode_num;
        } else {
            // Entry doesn't exist, create it
            if (next_token == NULL) {
                // This is the final component
                int new_inode = find_free_inode();
                if (new_inode < 0) {
                    return -1;
                }
                
                // Initialize directory inode
                inode_t dir_inode;
                memset(&dir_inode, 0, sizeof(inode_t));
                dir_inode.type = TYPE_DIR;
                
                // Allocate first data block for directory entries
                int data_block = find_free_data_block();
                if (data_block < 0) {
                    return -1;
                }
                
                dir_inode.direct_blocks[0] = data_block;
                
                // Write inode
                if (write_inode(new_inode, &dir_inode) != 0) {
                    return -1;
                }
                
                // Initialize directory block
                if (init_directory_block(data_block) != 0) {
                    return -1;
                }
                
                // Add entry to parent directory
                if (add_dir_entry(current_dir_inode, token, new_inode, TYPE_DIR) != 0) {
                    return -1;
                }
                
                return 0;
            } else {
                // Intermediate directory needs to be created
                int new_inode = find_free_inode();
                if (new_inode < 0) {
                    return -1;
                }
                
                // Initialize directory inode
                inode_t dir_inode;
                memset(&dir_inode, 0, sizeof(inode_t));
                dir_inode.type = TYPE_DIR;
                
                // Allocate first data block for directory entries
                int data_block = find_free_data_block();
                if (data_block < 0) {
                    return -1;
                }
                
                dir_inode.direct_blocks[0] = data_block;
                
                // Write inode
                if (write_inode(new_inode, &dir_inode) != 0) {
                    return -1;
                }
                
                // Initialize directory block
                if (init_directory_block(data_block) != 0) {
                    return -1;
                }
                
                // Add entry to parent directory
                if (add_dir_entry(current_dir_inode, token, new_inode, TYPE_DIR) != 0) {
                    return -1;
                }
                
                current_dir_inode = new_inode;
            }
        }
        
        token = next_token;
    }
    
    return 0;
}

// Split a path into directory path and filename
void split_path(const char* full_path, char* dir_path, char* filename) {
    char path_copy[MAX_PATH_LENGTH];
    strncpy(path_copy, full_path, MAX_PATH_LENGTH - 1);
    path_copy[MAX_PATH_LENGTH - 1] = '\0';
    
    // Find last slash
    char* last_slash = strrchr(path_copy, '/');
    
    if (last_slash == NULL) {
        // No slash found
        strcpy(dir_path, "/");
        strcpy(filename, path_copy);
    } else {
        // Split path
        *last_slash = '\0';
        if (path_copy[0] == '\0') {
            // Root directory
            strcpy(dir_path, "/");
        } else {
            strcpy(dir_path, path_copy);
        }
        strcpy(filename, last_slash + 1);
    }
}

// Get inode number for a path
int get_inode_for_path(const char* path) {
    // Start at root directory
    int current_inode = 0;  // Root directory is always inode 0
    
    // Root directory special case
    if (strcmp(path, "/") == 0) {
        return current_inode;
    }
    
    // Make a copy of the path to tokenize
    char path_copy[MAX_PATH_LENGTH];
    strncpy(path_copy, path, MAX_PATH_LENGTH - 1);
    path_copy[MAX_PATH_LENGTH - 1] = '\0';
    
    // Remove trailing slash if present
    int len = strlen(path_copy);
    if (len > 1 && path_copy[len - 1] == '/') {
        path_copy[len - 1] = '\0';
    }
    
    // Tokenize path
    char* saveptr;
    char* token = strtok_r(path_copy + 1, "/", &saveptr);  // Skip leading slash
    
    while (token != NULL) {
        dirent_t entry;
        if (find_dir_entry(current_inode, token, &entry) != 0) {
            // Entry not found
            return -1;
        }
        
        current_inode = entry.inode_num;
        token = strtok_r(NULL, "/", &saveptr);
    }
    
    return current_inode;
}

// Add a file to the file system
int add_file(const char* fs_path, const char* local_path) {
    // Create parent directories
    char dir_path[MAX_PATH_LENGTH];
    char filename[MAX_FILENAME_LENGTH];
    split_path(fs_path, dir_path, filename);
    
    if (create_directory(dir_path) != 0) {
        return -1;
    }
    
    // Get parent directory inode
    int dir_inode = get_inode_for_path(dir_path);
    if (dir_inode < 0) {
        return -1;
    }
    
    // Check if file already exists
    dirent_t existing_entry;
    if (find_dir_entry(dir_inode, filename, &existing_entry) == 0) {
        // File or directory already exists
        return -1;
    }
    
    // Create file inode
    int file_inode = find_free_inode();
    if (file_inode < 0) {
        return -1;
    }
    
    // Open local file
    FILE* local_file = fopen(local_path, "rb");
    if (local_file == NULL) {
        return -1;
    }
    
    // Get file size
    fseek(local_file, 0, SEEK_END);
    long file_size = ftell(local_file);
    fseek(local_file, 0, SEEK_SET);
    
    // Initialize file inode
    inode_t inode;
    memset(&inode, 0, sizeof(inode_t));
    inode.type = TYPE_FILE;
    inode.size = file_size;
    
    // Read file data and store in blocks
    long bytes_read = 0;
    int block_index = 0;
    
    while (bytes_read < file_size) {
        // Allocate a new data block
        int block_num = find_free_data_block();
        if (block_num < 0) {
            fclose(local_file);
            return -1;
        }
        
        if (block_index < MAX_DIRECT_BLOCKS) {
            // Store in direct blocks
            inode.direct_blocks[block_index] = block_num;
        } else if (block_index == MAX_DIRECT_BLOCKS) {
            // Allocate single indirect block if not already allocated
            if (inode.single_indirect == 0) {
                int indirect_block = find_free_data_block();
                if (indirect_block < 0) {
                    fclose(local_file);
                    return -1;
                }
                
                // Initialize indirect block with zeros
                uint32_t indirect_pointers[BLOCK_SIZE / sizeof(uint32_t)] = {0};
                if (write_data_block(indirect_block, indirect_pointers) != 0) {
                    fclose(local_file);
                    return -1;
                }
                
                inode.single_indirect = indirect_block;
            }
            
            // Read indirect block
            uint32_t indirect_pointers[BLOCK_SIZE / sizeof(uint32_t)];
            if (read_data_block(inode.single_indirect, indirect_pointers) != 0) {
                fclose(local_file);
                return -1;
            }
            
            // Store block pointer in indirect block
            int indirect_index = block_index - MAX_DIRECT_BLOCKS;
            if (indirect_index >= BLOCK_SIZE / sizeof(uint32_t)) {
                // Exceeded single indirect capacity
                fclose(local_file);
                return -1;
            }
            
            indirect_pointers[indirect_index] = block_num;
            
            // Write updated indirect block
            if (write_data_block(inode.single_indirect, indirect_pointers) != 0) {
                fclose(local_file);
                return -1;
            }
        } else {
            int indirect_index = block_index - MAX_DIRECT_BLOCKS;
            int single_indirect_size = BLOCK_SIZE / sizeof(uint32_t);
            
            if (indirect_index < single_indirect_size) {
                // Should have been handled in previous case
                fclose(local_file);
                return -1;
            } else if (indirect_index >= single_indirect_size && 
                       indirect_index < single_indirect_size + single_indirect_size * single_indirect_size) {
                // Double indirect territory
                int double_indirect_index = indirect_index - single_indirect_size;
                int primary_index = double_indirect_index / single_indirect_size;
                int secondary_index = double_indirect_index % single_indirect_size;
                
                // Allocate double indirect block if not already allocated
                if (inode.double_indirect == 0) {
                    int double_indirect_block = find_free_data_block();
                    if (double_indirect_block < 0) {
                        fclose(local_file);
                        return -1;
                    }
                    
                    // Initialize double indirect block with zeros
                    uint32_t double_indirect_pointers[single_indirect_size] = {0};
                    if (write_data_block(double_indirect_block, double_indirect_pointers) != 0) {
                        fclose(local_file);
                        return -1;
                    }
                    
                    inode.double_indirect = double_indirect_block;
                }
                
                // Read double indirect block
                uint32_t double_indirect_pointers[single_indirect_size];
                if (read_data_block(inode.double_indirect, double_indirect_pointers) != 0) {
                    fclose(local_file);
                    return -1;
                }
                
                // Allocate primary indirect block if needed
                if (double_indirect_pointers[primary_index] == 0) {
                    int primary_indirect_block = find_free_data_block();
                    if (primary_indirect_block < 0) {
                        fclose(local_file);
                        return -1;
                    }
                    
                    // Initialize primary indirect block with zeros
                    uint32_t primary_indirect_pointers[single_indirect_size] = {0};
                    if (write_data_block(primary_indirect_block, primary_indirect_pointers) != 0) {
                        fclose(local_file);
                        return -1;
                    }
                    
                    double_indirect_pointers[primary_index] = primary_indirect_block;
                    
                    // Update double indirect block
                    if (write_data_block(inode.double_indirect, double_indirect_pointers) != 0) {
                        fclose(local_file);
                        return -1;
                    }
                }
                
                // Read primary indirect block
                uint32_t primary_indirect_pointers[single_indirect_size];
                if (read_data_block(double_indirect_pointers[primary_index], primary_indirect_pointers) != 0) {
                    fclose(local_file);
                    return -1;
                }
                
                // Store block pointer in primary indirect block
                primary_indirect_pointers[secondary_index] = block_num;
                
                // Write updated primary indirect block
                if (write_data_block(double_indirect_pointers[primary_index], primary_indirect_pointers) != 0) {
                    fclose(local_file);
                    return -1;
                }
            } else {
                // Triple indirect blocks not implemented
                fclose(local_file);
                return -1;
            }
        }
        
        // Read data from local file
        char buffer[BLOCK_SIZE];
        memset(buffer, 0, BLOCK_SIZE);
        size_t read_size = fread(buffer, 1, BLOCK_SIZE, local_file);
        
        // Write data block
        if (write_data_block(block_num, buffer) != 0) {
            fclose(local_file);
            return -1;
        }
        
        bytes_read += read_size;
        block_index++;
    }
    
    fclose(local_file);
    
    // Write file inode
    if (write_inode(file_inode, &inode) != 0) {
        return -1;
    }
    
    // Add entry to parent directory
    if (add_dir_entry(dir_inode, filename, file_inode, TYPE_FILE) != 0) {
        return -1;
    }
    
    return 0;
}

// Extract a file from the file system to stdout
int extract_file(const char* fs_path) {
    int inode_num = get_inode_for_path(fs_path);
    if (inode_num < 0) {
        return -1;
    }
    
    inode_t inode;
    if (read_inode(inode_num, &inode) != 0) {
        return -1;
    }
    
    if (inode.type != TYPE_FILE) {
        return -1;  // Not a file
    }
    
    // Extract file data
    long bytes_written = 0;
    int block_index = 0;
    
    while (bytes_written < inode.size && block_index < MAX_DIRECT_BLOCKS) {
        if (inode.direct_blocks[block_index] == 0) {
            break;
        }
        
        // Read data block
        char buffer[BLOCK_SIZE];
        if (read_data_block(inode.direct_blocks[block_index], buffer) != 0) {
            return -1;
        }
        
        // Calculate how much to write from this block
        long remaining = inode.size - bytes_written;
        long to_write = (remaining < BLOCK_SIZE) ? remaining : BLOCK_SIZE;
        
        // Write to stdout
        if (fwrite(buffer, 1, to_write, stdout) != to_write) {
            return -1;
        }
        
        bytes_written += to_write;
        block_index++;
    }
    
    // Handle single indirect blocks
    if (bytes_written < inode.size && inode.single_indirect != 0) {
        // Read indirect block
        uint32_t indirect_pointers[BLOCK_SIZE / sizeof(uint32_t)];
        if (read_data_block(inode.single_indirect, indirect_pointers) != 0) {
            return -1;
        }
        
        // Read data blocks referenced by indirect block
        for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t) && bytes_written < inode.size; i++) {
            if (indirect_pointers[i] == 0) {
                break;
            }
            
            // Read data block
            char buffer[BLOCK_SIZE];
            if (read_data_block(indirect_pointers[i], buffer) != 0) {
                return -1;
            }
            
            // Calculate how much to write from this block
            long remaining = inode.size - bytes_written;
            long to_write = (remaining < BLOCK_SIZE) ? remaining : BLOCK_SIZE;
            
            // Write to stdout
            if (fwrite(buffer, 1, to_write, stdout) != to_write) {
                return -1;
            }
            
            bytes_written += to_write;
        }
    }
    
    // Handle double indirect blocks
    if (bytes_written < inode.size && inode.double_indirect != 0) {
        // Read double indirect block
        int single_indirect_size = BLOCK_SIZE / sizeof(uint32_t);
        uint32_t double_indirect_pointers[single_indirect_size];
        
        if (read_data_block(inode.double_indirect, double_indirect_pointers) != 0) {
            return -1;
        }
        
        // Read primary indirect blocks
        for (int i = 0; i < single_indirect_size && bytes_written < inode.size; i++) {
            if (double_indirect_pointers[i] == 0) {
                continue;
            }
            
            // Read primary indirect block
            uint32_t primary_indirect_pointers[single_indirect_size];
            if (read_data_block(double_indirect_pointers[i], primary_indirect_pointers) != 0) {
                continue;
            }
            
            // Read data blocks referenced by primary indirect block
            for (int j = 0; j < single_indirect_size && bytes_written < inode.size; j++) {
                if (primary_indirect_pointers[j] == 0) {
                    continue;
                }
                
                // Read data block
                char buffer[BLOCK_SIZE];
                if (read_data_block(primary_indirect_pointers[j], buffer) != 0) {
                    continue;
                }
                
                // Calculate how much to write from this block
                long remaining = inode.size - bytes_written;
                long to_write = (remaining < BLOCK_SIZE) ? remaining : BLOCK_SIZE;
                
                // Write to stdout
                if (fwrite(buffer, 1, to_write, stdout) != to_write) {
                    return -1;
                }
                
                bytes_written += to_write;
            }
        }
    }
    
    // Triple indirect blocks would be handled similarly
    
    return 0;
}

// Recursive function to list directory contents
void list_directory(int dir_inode, int depth) {
    inode_t dir_inode_data;
    if (read_inode(dir_inode, &dir_inode_data) != 0) {
        return;
    }
    
    if (dir_inode_data.type != TYPE_DIR) {
        return;  // Not a directory
    }
    
    // Check direct blocks
    for (int i = 0; i < MAX_DIRECT_BLOCKS && dir_inode_data.direct_blocks[i] != 0; i++) {
        dirblock_t dir_block;
        if (read_data_block(dir_inode_data.direct_blocks[i], &dir_block) != 0) {
            continue;
        }
        
        for (int j = 0; j < MAX_ENTRIES_PER_DIR; j++) {
            if (dir_block.entries[j].inuse) {
                // Print information about current entry
        if (entry.type == TYPE_DIR) {
            printf("directory '%s':\n", token);
            
            current_inode = entry.inode_num;
            inode_t dir_inode;
            if (read_inode(current_inode, &dir_inode) != 0) {
                return -1;
            }
            
            // List directory entries
            for (int i = 0; i < MAX_DIRECT_BLOCKS && dir_inode.direct_blocks[i] != 0; i++) {
                dirblock_t dir_block;
                if (read_data_block(dir_inode.direct_blocks[i], &dir_block) != 0) {
                    continue;
                }
                
                for (int j = 0; j < MAX_ENTRIES_PER_DIR; j++) {
                    if (dir_block.entries[j].inuse) {
                        printf("'%s' %d\n", dir_block.entries[j].name, dir_block.entries[j].inode_num);
                    }
                }
            }
        } else {
            printf("regular file '%s' inode %d\n", token, entry.inode_num);
            return 0;
        }
        
        token = strtok_r(NULL, "/", &saveptr);
    }
    
    return 0;
}

void print_usage() {
    printf("Usage: exfs2 [options]\n");
    printf("Options:\n");
    printf("  -l                       List the contents of the file system\n");
    printf("  -a <fs_path> -f <local_path>  Add a file to the file system\n");
    printf("  -r <fs_path>             Remove a file or directory from the file system\n");
    printf("  -e <fs_path>             Extract a file from the file system to stdout\n");
    printf("  -D <fs_path>             Debug a path in the file system\n");
    printf("  -h                       Display this help message\n");
}

int main(int argc, char* argv[]) {
    // Ensure inodeseg0 exists (with root directory)
    if (create_segment_if_not_exists("inodeseg0") != 0) {
        fprintf(stderr, "Error initializing file system\n");
        return 1;
    }
    
    // Parse command line arguments
    if (argc < 2) {
        print_usage();
        return 1;
    }
    
    if (strcmp(argv[1], "-l") == 0) {
        // List file system contents
        return list_fs();
    } else if (strcmp(argv[1], "-a") == 0 && argc >= 5 && strcmp(argv[3], "-f") == 0) {
        // Add file to file system
        return add_file(argv[2], argv[4]);
    } else if (strcmp(argv[1], "-r") == 0 && argc >= 3) {
        // Remove file from file system
        return remove_file(argv[2]);
    } else if (strcmp(argv[1], "-e") == 0 && argc >= 3) {
        // Extract file from file system
        return extract_file(argv[2]);
    } else if (strcmp(argv[1], "-D") == 0 && argc >= 3) {
        // Debug path
        return debug_path(argv[2]);
    } else {
        print_usage();
        return 1;
    }
    
    return 0;
} entry
                for (int k = 0; k < depth; k++) {
                    printf("\t");
                }
                
                printf("%s", dir_block.entries[j].name);
                
                if (dir_block.entries[j].type == TYPE_DIR) {
                    printf("/\n");
                    list_directory(dir_block.entries[j].inode_num, depth + 1);
                } else {
                    printf("\n");
                }
            }
        }
    }
}

// List the file system contents
int list_fs() {
    // Start at root directory
    printf("/\n");
    list_directory(0, 1);
    return 0;
}

// Free data blocks used by an inode
int free_data_blocks(inode_t* inode) {
    // Free direct blocks
    for (int i = 0; i < MAX_DIRECT_BLOCKS; i++) {
        if (inode->direct_blocks[i] != 0) {
            int segment_num, offset;
            if (get_data_segment_and_offset(inode->direct_blocks[i], &segment_num, &offset) == 0) {
                char segment_name[20];
                sprintf(segment_name, "dataseg%d", segment_num);
                
                FILE* file = fopen(segment_name, "rb+");
                if (file != NULL) {
                    // Read bitmap
                    unsigned char bitmap[BLOCK_SIZE];
                    fread(bitmap, 1, BLOCK_SIZE, file);
                    
                    // Mark block as free
                    bitmap[offset / 8] &= ~(1 << (offset % 8));
                    
                    // Write updated bitmap
                    fseek(file, 0, SEEK_SET);
                    fwrite(bitmap, 1, BLOCK_SIZE, file);
                    fclose(file);
                }
            }
        }
    }
    
    // Free single indirect block and its data blocks
    if (inode->single_indirect != 0) {
        // Read indirect block
        uint32_t indirect_pointers[BLOCK_SIZE / sizeof(uint32_t)];
        if (read_data_block(inode->single_indirect, indirect_pointers) == 0) {
            // Free data blocks referenced by indirect block
            for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t); i++) {
                if (indirect_pointers[i] != 0) {
                    int segment_num, offset;
                    if (get_data_segment_and_offset(indirect_pointers[i], &segment_num, &offset) == 0) {
                        char segment_name[20];
                        sprintf(segment_name, "dataseg%d", segment_num);
                        
                        FILE* file = fopen(segment_name, "rb+");
                        if (file != NULL) {
                            // Read bitmap
                            unsigned char bitmap[BLOCK_SIZE];
                            fread(bitmap, 1, BLOCK_SIZE, file);
                            
                            // Mark block as free
                            bitmap[offset / 8] &= ~(1 << (offset % 8));
                            
                            // Write updated bitmap
                            fseek(file, 0, SEEK_SET);
                            fwrite(bitmap, 1, BLOCK_SIZE, file);
                            fclose(file);
                        }
                    }
                }
            }
        }
        
        // Free the indirect block itself
        int segment_num, offset;
        if (get_data_segment_and_offset(inode->single_indirect, &segment_num, &offset) == 0) {
            char segment_name[20];
            sprintf(segment_name, "dataseg%d", segment_num);
            
            FILE* file = fopen(segment_name, "rb+");
            if (file != NULL) {
                // Read bitmap
                unsigned char bitmap[BLOCK_SIZE];
                fread(bitmap, 1, BLOCK_SIZE, file);
                
                // Mark block as free
                bitmap[offset / 8] &= ~(1 << (offset % 8));
                
                // Write updated bitmap
                fseek(file, 0, SEEK_SET);
                fwrite(bitmap, 1, BLOCK_SIZE, file);
                fclose(file);
            }
        }
    }
    
    // Handle double indirect blocks
    if (inode->double_indirect != 0) {
        // Read double indirect block
        int single_indirect_size = BLOCK_SIZE / sizeof(uint32_t);
        uint32_t double_indirect_pointers[single_indirect_size];
        
        if (read_data_block(inode->double_indirect, double_indirect_pointers) == 0) {
            // Free each primary indirect block and its data blocks
            for (int i = 0; i < single_indirect_size; i++) {
                if (double_indirect_pointers[i] != 0) {
                    // Read primary indirect block
                    uint32_t primary_indirect_pointers[single_indirect_size];
                    if (read_data_block(double_indirect_pointers[i], primary_indirect_pointers) == 0) {
                        // Free data blocks referenced by primary indirect block
                        for (int j = 0; j < single_indirect_size; j++) {
                            if (primary_indirect_pointers[j] != 0) {
                                int segment_num, offset;
                                if (get_data_segment_and_offset(primary_indirect_pointers[j], &segment_num, &offset) == 0) {
                                    char segment_name[20];
                                    sprintf(segment_name, "dataseg%d", segment_num);
                                    
                                    FILE* file = fopen(segment_name, "rb+");
                                    if (file != NULL) {
                                        // Read bitmap
                                        unsigned char bitmap[BLOCK_SIZE];
                                        fread(bitmap, 1, BLOCK_SIZE, file);
                                        
                                        // Mark block as free
                                        bitmap[offset / 8] &= ~(1 << (offset % 8));
                                        
                                        // Write updated bitmap
                                        fseek(file, 0, SEEK_SET);
                                        fwrite(bitmap, 1, BLOCK_SIZE, file);
                                        fclose(file);
                                    }
                                }
                            }
                        }
                    }
                    
                    // Free the primary indirect block itself
                    int segment_num, offset;
                    if (get_data_segment_and_offset(double_indirect_pointers[i], &segment_num, &offset) == 0) {
                        char segment_name[20];
                        sprintf(segment_name, "dataseg%d", segment_num);
                        
                        FILE* file = fopen(segment_name, "rb+");
                        if (file != NULL) {
                            // Read bitmap
                            unsigned char bitmap[BLOCK_SIZE];
                            fread(bitmap, 1, BLOCK_SIZE, file);
                            
                            // Mark block as free
                            bitmap[offset / 8] &= ~(1 << (offset % 8));
                            
                            // Write updated bitmap
                            fseek(file, 0, SEEK_SET);
                            fwrite(bitmap, 1, BLOCK_SIZE, file);
                            fclose(file);
                        }
                    }
                }
            }
        }
        
        // Free the double indirect block itself
        int segment_num, offset;
        if (get_data_segment_and_offset(inode->double_indirect, &segment_num, &offset) == 0) {
            char segment_name[20];
            sprintf(segment_name, "dataseg%d", segment_num);
            
            FILE* file = fopen(segment_name, "rb+");
            if (file != NULL) {
                // Read bitmap
                unsigned char bitmap[BLOCK_SIZE];
                fread(bitmap, 1, BLOCK_SIZE, file);
                
                // Mark block as free
                bitmap[offset / 8] &= ~(1 << (offset % 8));
                
                // Write updated bitmap
                fseek(file, 0, SEEK_SET);
                fwrite(bitmap, 1, BLOCK_SIZE, file);
                fclose(file);
            }
        }
    }
    
    // Triple indirect blocks would be handled similarly
    
    return 0;
}

// Free an inode
int free_inode(int inode_num) {
    int segment_num, offset;
    if (get_inode_segment_and_offset(inode_num, &segment_num, &offset) != 0) {
        return -1;
    }
    
    char segment_name[20];
    sprintf(segment_name, "inodeseg%d", segment_num);
    
    FILE* file = fopen(segment_name, "rb+");
    if (file == NULL) {
        return -1;
    }
    
    // Read bitmap
    unsigned char bitmap[BLOCK_SIZE];
    fread(bitmap, 1, BLOCK_SIZE, file);
    
    // Mark inode as free
    bitmap[offset / 8] &= ~(1 << (offset % 8));
    
    // Write updated bitmap
    fseek(file, 0, SEEK_SET);
    fwrite(bitmap, 1, BLOCK_SIZE, file);
    fclose(file);
    
    return 0;
}

// Remove an entry from a directory
int remove_dir_entry(int dir_inode_num, const char* name) {
    inode_t dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0) {
        return -1;
    }
    
    if (dir_inode.type != TYPE_DIR) {
        return -1;  // Not a directory
    }
    
    // Check direct blocks
    for (int i = 0; i < MAX_DIRECT_BLOCKS && dir_inode.direct_blocks[i] != 0; i++) {
        dirblock_t dir_block;
        if (read_data_block(dir_inode.direct_blocks[i], &dir_block) != 0) {
            continue;
        }
        
        for (int j = 0; j < MAX_ENTRIES_PER_DIR; j++) {
            if (dir_block.entries[j].inuse && strcmp(dir_block.entries[j].name, name) == 0) {
                // Found entry, mark as unused
                dir_block.entries[j].inuse = 0;
                
                // Write updated directory block
                if (write_data_block(dir_inode.direct_blocks[i], &dir_block) != 0) {
                    return -1;
                }
                
                return 0;
            }
        }
    }
    
    // TODO: Check indirect blocks
    
    return -1;  // Entry not found
}

// Check if a directory is empty
int is_dir_empty(int dir_inode_num) {
    inode_t dir_inode;
    if (read_inode(dir_inode_num, &dir_inode) != 0) {
        return 0;  // Error, assume not empty
    }
    
    if (dir_inode.type != TYPE_DIR) {
        return 0;  // Not a directory
    }
    
    // Check direct blocks
    for (int i = 0; i < MAX_DIRECT_BLOCKS && dir_inode.direct_blocks[i] != 0; i++) {
        dirblock_t dir_block;
        if (read_data_block(dir_inode.direct_blocks[i], &dir_block) != 0) {
            continue;
        }
        
        for (int j = 0; j < MAX_ENTRIES_PER_DIR; j++) {
            if (dir_block.entries[j].inuse) {
                return 0;  // Directory not empty
            }
        }
    }
    
    // TODO: Check indirect blocks
    
    return 1;  // Directory is empty
}

// Remove a file or directory from the file system
int remove_file(const char* fs_path) {
    // Special case: can't remove root directory
    if (strcmp(fs_path, "/") == 0) {
        return -1;
    }
    
    // Get parent directory path and filename
    char dir_path[MAX_PATH_LENGTH];
    char filename[MAX_FILENAME_LENGTH];
    split_path(fs_path, dir_path, filename);
    
    // Get parent directory inode
    int dir_inode = get_inode_for_path(dir_path);
    if (dir_inode < 0) {
        return -1;
    }
    
    // Find entry in parent directory
    dirent_t entry;
    if (find_dir_entry(dir_inode, filename, &entry) != 0) {
        return -1;  // Entry not found
    }
    
    // Get inode for the file/directory to be removed
    inode_t inode;
    if (read_inode(entry.inode_num, &inode) != 0) {
        return -1;
    }
    
    if (inode.type == TYPE_DIR) {
        // Check if directory is empty
        if (!is_dir_empty(entry.inode_num)) {
            return -1;  // Can't remove non-empty directory
        }
        
        // Free data blocks used by directory
        free_data_blocks(&inode);
        
        // Free directory inode
        free_inode(entry.inode_num);
        
        // Remove entry from parent directory
        remove_dir_entry(dir_inode, filename);
    } else {
        // Free data blocks used by file
        free_data_blocks(&inode);
        
        // Free file inode
        free_inode(entry.inode_num);
        
        // Remove entry from parent directory
        remove_dir_entry(dir_inode, filename);
    }
    
    return 0;
}

// Debug function to print information about a path
int debug_path(const char* fs_path) {
    // Start at root directory
    int current_inode = 0;  // Root directory is always inode 0
    
    // Make a copy of the path to tokenize
    char path_copy[MAX_PATH_LENGTH];
    strncpy(path_copy, fs_path, MAX_PATH_LENGTH - 1);
    path_copy[MAX_PATH_LENGTH - 1] = '\0';
    
    // Remove trailing slash if present
    int len = strlen(path_copy);
    if (len > 1 && path_copy[len - 1] == '/') {
        path_copy[len - 1] = '\0';
    }
    
    // Root directory special case
    if (strcmp(path_copy, "/") == 0) {
        printf("directory '/':\n");
        
        inode_t root_inode;
        if (read_inode(current_inode, &root_inode) != 0) {
            return -1;
        }
        
        // List directory entries
        for (int i = 0; i < MAX_DIRECT_BLOCKS && root_inode.direct_blocks[i] != 0; i++) {
            dirblock_t dir_block;
            if (read_data_block(root_inode.direct_blocks[i], &dir_block) != 0) {
                continue;
            }
            
            for (int j = 0; j < MAX_ENTRIES_PER_DIR; j++) {
                if (dir_block.entries[j].inuse) {
                    printf("'%s' %d\n", dir_block.entries[j].name, dir_block.entries[j].inode_num);
                }
            }
        }
        
        return 0;
    }
    
    // Tokenize path
    char* saveptr;
    char* token = strtok_r(path_copy + 1, "/", &saveptr);  // Skip leading slash
    
    printf("directory '/':\n");
    
    while (token != NULL) {
        dirent_t entry;
        if (find_dir_entry(current_inode, token, &entry) != 0) {
            // Entry not found
            return -1;
        }
        
        // Print