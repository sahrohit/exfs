#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>  // for exit()

#define BLOCK_SIZE 4096
#define TOTAL_BLOCKS 1024
#define TYPE_FILE 0
#define TYPE_DIR 1
#define MAX_NAME_LEN 256

int allocate_inode();
int allocate_data_block();
int lookup_path(const char *path, int create_missing);

typedef struct {
    uint8_t type;
    uint32_t size;
    uint32_t direct_pointers[12];
    uint32_t indirect_pointers;
} inode_field;

typedef union {
    inode_field access;
    uint8_t raw[BLOCK_SIZE];
} inode;

typedef struct {
    uint32_t inode_number;
    char name[MAX_NAME_LEN];
} dir_entry;

void bitmap_set(uint8_t *bitmap, int bit_num) {
    int byte_index = bit_num / 8;
    int bit_index = bit_num % 8;
    bitmap[byte_index] |= (1 << bit_index);
}

void bitmap_clear(uint8_t *bitmap, int bit_num) {
    int byte_index = bit_num / 8;
    int bit_index = bit_num % 8;
    bitmap[byte_index] &= ~(1 << bit_index);
}

int bitmap_is_set(uint8_t *bitmap, int bit_num) {
    int byte_index = bit_num / 8;
    int bit_index = bit_num % 8;
    return (bitmap[byte_index] & (1 << bit_index)) != 0;
}

int bitmap_find_free(uint8_t *bitmap, int size_in_bits) {
    for (int i = 0; i < size_in_bits; i++) {
        if (!bitmap_is_set(bitmap, i)) {
            return i;
        }
    }
    return -1; // No free bit found
}

// Write a 4096-byte block into a segment file
void write_block(const char *filename, int block_num, const uint8_t *data) {
    FILE *fp = fopen(filename, "rb+");
    if (!fp) {
        perror("Error opening file to write block");
        return;
    }

    if (fseek(fp, block_num * BLOCK_SIZE, SEEK_SET) != 0) {
        perror("Error seeking to block");
        fclose(fp);
        return;
    }

    size_t written = fwrite(data, 1, BLOCK_SIZE, fp);
    if (written != BLOCK_SIZE) {
        perror("Error writing block");
    }

    fclose(fp);
}

// ===== ✅ NEW FUNCTION: Read a 4096-byte block from a segment file =====
void read_block(const char *filename, int block_num, uint8_t *buffer) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("Error opening file to read block");
        return;
    }

    if (fseek(fp, block_num * BLOCK_SIZE, SEEK_SET) != 0) {
        perror("Error seeking to block for reading");
        fclose(fp);
        return;
    }

    size_t read = fread(buffer, 1, BLOCK_SIZE, fp);
    if (read != BLOCK_SIZE) {
        perror("Error reading block");
    }

    fclose(fp);
}
// ======================================================================

// ===== ✅ NEW FUNCTION: Split a path into components =====
int split_path(const char *path, char ***components_out) {
    char *path_copy = strdup(path);
    if (!path_copy) {
        perror("strdup failed");
        return -1;
    }

    int count = 0;
    char *token;
    char **components = NULL;

    // First pass: count components
    token = strtok(path_copy, "/");
    while (token != NULL) {
        count++;
        token = strtok(NULL, "/");
    }

    free(path_copy);

    // Allocate array
    components = (char **)malloc(sizeof(char *) * count);
    if (!components) {
        perror("malloc failed");
        return -1;
    }

    // Second pass: copy components
    path_copy = strdup(path);
    int i = 0;
    token = strtok(path_copy, "/");
    while (token != NULL) {
        components[i] = strdup(token);
        if (!components[i]) {
            perror("strdup failed for component");
            // Free previously allocated components
            for (int j = 0; j < i; j++) {
                free(components[j]);
            }
            free(components);
            return -1;
        }
        i++;
        token = strtok(NULL, "/");
    }

    free(path_copy);

    *components_out = components;
    return count;
}
// ======================================================================

// ===== ✅ NEW FUNCTION: Lookup path and find final inode number =====
int lookup_path(const char *path, int create_missing) {
    printf("Looking up path: '%s' (create_missing=%d)\n", path, create_missing);
    
    // Special case for root directory
    if (strcmp(path, "/") == 0) {
        printf("Root directory requested, returning inode 1\n");
        return 1;  // Root inode
    }
    
    char **components;
    int count = split_path(path, &components);
    if (count < 0) {
        printf("Error splitting path\n");
        return -1;
    }
    
    printf("Path split into %d components:\n", count);
    for (int i = 0; i < count; i++) {
        printf("  Component %d: '%s'\n", i, components[i]);
    }

    uint8_t inode_buffer[BLOCK_SIZE];
    read_block("inode_seg0.bin", 1, inode_buffer); // Start from root inode (inode #1)
    inode_field *current_inode = (inode_field *)inode_buffer;
    int current_inode_number = 1; // Inode 1 is root

    printf("Starting traversal from root inode %d\n", current_inode_number);

    for (int level = 0; level < count; level++) {
        printf("Looking for component '%s' in directory (inode %d)\n", 
               components[level], current_inode_number);
        
        if (current_inode->type != TYPE_DIR) {
            printf("Error: inode %d is not a directory!\n", current_inode_number);
            for (int j = 0; j < count; j++) free(components[j]);
            free(components);
            return -1;
        }
        
        uint8_t dir_block[BLOCK_SIZE];
        uint32_t block_num = current_inode->direct_pointers[0];
        
        if (block_num == 0) {
            printf("Error: directory inode %d has no data blocks!\n", current_inode_number);
            for (int j = 0; j < count; j++) free(components[j]);
            free(components);
            return -1;
        }
        
        read_block("data_seg0.bin", block_num, dir_block);
        dir_entry *entries = (dir_entry *)dir_block;

        int found = 0;
        for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
            if (entries[i].inode_number != 0) {
                printf("  Dir entry %d: inode=%d, name='%s'\n", 
                       i, entries[i].inode_number, entries[i].name);
                
                if (strcmp(entries[i].name, components[level]) == 0) {
                    found = 1;
                    current_inode_number = entries[i].inode_number;
                    printf("  Found match! Moving to inode %d\n", current_inode_number);
                    read_block("inode_seg0.bin", current_inode_number, inode_buffer);
                    current_inode = (inode_field *)inode_buffer;
                    break;
                }
            }
        }

        if (!found) {
            printf("Component '%s' not found in directory.\n", components[level]);
            if (create_missing) {
                printf("Creating missing directory: %s\n", components[level]);

                // Allocate new inode
                int new_inode_num = allocate_inode();
                if (new_inode_num < 0) {
                    printf("No free inodes available!\n");
                    for (int j = 0; j < count; j++) free(components[j]);
                    free(components);
                    return -1;
                }

                // Allocate new data block
                int new_data_block = allocate_data_block();
                if (new_data_block < 0) {
                    printf("No free data blocks available!\n");
                    for (int j = 0; j < count; j++) free(components[j]);
                    free(components);
                    return -1;
                }

                // Create new directory inode
                inode_field new_dir_inode = {0};
                new_dir_inode.type = TYPE_DIR;
                new_dir_inode.size = sizeof(dir_entry) * 2; // . and ..
                new_dir_inode.direct_pointers[0] = new_data_block;
                write_block("inode_seg0.bin", new_inode_num, (uint8_t *)&new_dir_inode);

                // Initialize directory block
                uint8_t new_dir_block[BLOCK_SIZE] = {0};
                dir_entry *new_entries = (dir_entry *)new_dir_block;

                new_entries[0].inode_number = new_inode_num;
                strcpy(new_entries[0].name, ".");
                new_entries[1].inode_number = current_inode_number;
                strcpy(new_entries[1].name, "..");

                write_block("data_seg0.bin", new_data_block, new_dir_block);

                printf("Directory '%s' created with inode %d\n", components[level], new_inode_num);

                // Add entry in current directory
                for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
                    if (entries[i].inode_number == 0) {
                        entries[i].inode_number = new_inode_num;
                        strncpy(entries[i].name, components[level], MAX_NAME_LEN - 1);
                        entries[i].name[MAX_NAME_LEN - 1] = '\0';
                        write_block("data_seg0.bin", block_num, dir_block);
                        break;
                    }
                }

                // Now move into the newly created directory
                current_inode_number = new_inode_num;
                read_block("inode_seg0.bin", current_inode_number, inode_buffer);
                current_inode = (inode_field *)inode_buffer;
            }
            else {
                printf("Component '%s' not found in path.\n", components[level]);
                for (int j = 0; j < count; j++) free(components[j]);
                free(components);
                return -1;
            }
        }
    }

    // Cleanup
    printf("Path lookup complete, found inode %d\n", current_inode_number);
    for (int i = 0; i < count; i++) free(components[i]);
    free(components);

    return current_inode_number;
}


// ======================================================================

void list_directory(const char *path) {
    // Step 1: Lookup the inode number for the path
    int inode_number = lookup_path(path,0);   
    if (inode_number < 0) {
        printf("Path '%s' not found.\n", path);
        return;
    }

    // Step 2: Read the inode
    uint8_t inode_buffer[BLOCK_SIZE];
    read_block("inode_seg0.bin", inode_number, inode_buffer);
    inode_field *dir_inode = (inode_field *)inode_buffer;

    // Step 3: Verify it's a directory
    if (dir_inode->type != TYPE_DIR) {
        printf("Error: '%s' is not a directory!\n", path);
        return;
    }

    printf("Directory listing for %s:\n", path);

    // Step 4: List entries in direct pointers
    for (int dp = 0; dp < 12; dp++) {
        if (dir_inode->direct_pointers[dp] == 0) continue;

        uint8_t dir_block[BLOCK_SIZE];
        read_block("data_seg0.bin", dir_inode->direct_pointers[dp], dir_block);
        dir_entry *entries = (dir_entry *)dir_block;

        for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
            if (entries[i].inode_number != 0 && entries[i].name[0] != '\0') {
                printf("- %s (inode %d)\n", entries[i].name, entries[i].inode_number);
            }
        }
    }

    // Step 5: List entries in indirect pointer if exists
    if (dir_inode->indirect_pointers != 0) {
        uint8_t indirect_block[BLOCK_SIZE];
        read_block("data_seg0.bin", dir_inode->indirect_pointers, indirect_block);
        uint32_t *indirect_entries = (uint32_t *)indirect_block;

        for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t); i++) {
            if (indirect_entries[i] == 0) continue;

            uint8_t dir_block[BLOCK_SIZE];
            read_block("data_seg0.bin", indirect_entries[i], dir_block);
            dir_entry *entries = (dir_entry *)dir_block;

            for (int j = 0; j < BLOCK_SIZE / sizeof(dir_entry); j++) {
                if (entries[j].inode_number != 0 && entries[j].name[0] != '\0') {
                    printf("- %s (inode %d)\n", entries[j].name, entries[j].inode_number);
                }
            }
        }
    }
}

// ======================================================================

void create_inode_segment(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Error creating inode segment");
        return;
    }

    uint8_t empty_block[BLOCK_SIZE] = {0};

    // Reserve inode 1 (bit 1)
    empty_block[0] |= (1 << 1);

    // Write bitmap to block 0
    fwrite(empty_block, 1, BLOCK_SIZE, fp);

    // Clear the rest of the segment (inodes themselves)
    memset(empty_block, 0, BLOCK_SIZE);
    for (int i = 1; i < TOTAL_BLOCKS; i++) {
        fwrite(empty_block, 1, BLOCK_SIZE, fp);
    }

    fclose(fp);
}

void create_data_segment(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Error creating data segment");
        return;
    }

    uint8_t empty_block[BLOCK_SIZE] = {0};

    // Reserve block 1 for root directory
    empty_block[0] |= (1 << 1);

    // Write bitmap to block 0
    fwrite(empty_block, 1, BLOCK_SIZE, fp);

    // Clear the rest of the segment (data blocks)
    memset(empty_block, 0, BLOCK_SIZE);
    for (int i = 1; i < TOTAL_BLOCKS; i++) {
        fwrite(empty_block, 1, BLOCK_SIZE, fp);
    }

    fclose(fp);
}


void create_root_inode(const char *filename) {
    inode_field root_inode = {0};
    root_inode.type = TYPE_DIR;
    root_inode.size = sizeof(dir_entry) * 2; // Size accounts for . and .. entries
    root_inode.direct_pointers[0] = 1; // block 1 holds root dir entries

    // Write root inode to block 1 (inode #1)
    write_block(filename, 1, (uint8_t *)&root_inode);
}

void init_directory_block(const char *filename, int block_num) {
    uint8_t dir_block[BLOCK_SIZE] = {0};
    dir_entry *entries = (dir_entry *)dir_block;

    // Initialize all entries to inode_number = 0 and empty name
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        entries[i].inode_number = 0;
        entries[i].name[0] = '\0';  // empty string
    }

    // Setup '.' entry to point to itself (inode #1 for root)
    entries[0].inode_number = 1;
    strcpy(entries[0].name, ".");

    // Setup '..' entry to also point to root for the root directory
    entries[1].inode_number = 1;
    strcpy(entries[1].name, "..");

    write_block(filename, block_num, dir_block);
}

void format_filesystem() {
    create_inode_segment("inode_seg0.bin");
    create_data_segment("data_seg0.bin");

    create_root_inode("inode_seg0.bin");
    printf("Root inode created in inode_seg0.bin\n");

    init_directory_block("data_seg0.bin", 1);
    printf("Root directory block initialized in data_seg0.bin\n");

    printf("Filesystem formatted: inode bitmap and data bitmap written successfully.\n");
}


int allocate_inode() {
    uint8_t bitmap[BLOCK_SIZE];
    read_block("inode_seg0.bin", 0, bitmap);

    // Start from inode 2 (skip inode 0 which is invalid and inode 1 which is root)
    for (int bit_num = 2; bit_num < BLOCK_SIZE * 8; bit_num++) {
        if (!bitmap_is_set(bitmap, bit_num)) {
            bitmap_set(bitmap, bit_num);
            write_block("inode_seg0.bin", 0, bitmap);
            printf("Allocated inode number: %d\n", bit_num);  // Debug output
            return bit_num;
        }
    }
    printf("No free inodes found!\n");  // Debug output
    return -1;  // No free inode found
}


int allocate_data_block() {
    uint8_t bitmap[BLOCK_SIZE];
    read_block("data_seg0.bin", 0, bitmap);

    int free_block = bitmap_find_free(bitmap, BLOCK_SIZE * 8);
    if (free_block != -1) {
        bitmap_set(bitmap, free_block);
        write_block("data_seg0.bin", 0, bitmap);
    }
    return free_block;
}


void add_entry_to_directory(int parent_inode_num, const char *file_name, int inode_num) {
    if (inode_num <= 0) {
        printf("Error: Cannot add entry with invalid inode number %d\n", inode_num);
        return;
    }

    uint8_t inode_buffer[BLOCK_SIZE];
    read_block("inode_seg0.bin", parent_inode_num, inode_buffer);
    inode_field *parent_inode = (inode_field *)inode_buffer;

    // Verify parent is a directory
    if (parent_inode->type != TYPE_DIR) {
        printf("Error: Cannot add entry to a non-directory inode %d\n", parent_inode_num);
        return;
    }

    // Debug info
    printf("Adding entry '%s' (inode %d) to directory (inode %d)\n", 
           file_name, inode_num, parent_inode_num);

    int found_free_slot = 0;
    
    // Iterate through direct pointers to find a space
    for (int dp = 0; dp < 12 && !found_free_slot; dp++) {
        uint32_t dir_block_num = parent_inode->direct_pointers[dp];
        
        // If no block allocated yet, allocate a new one
        if (dir_block_num == 0) {
            dir_block_num = allocate_data_block();
            if (dir_block_num < 0) {
                printf("No free data blocks for directory expansion!\n");
                return;
            }
            parent_inode->direct_pointers[dp] = dir_block_num;
            
            // Initialize new directory block
            uint8_t new_dir_block[BLOCK_SIZE] = {0};
            write_block("data_seg0.bin", dir_block_num, new_dir_block);
            
            // Update parent inode on disk
            write_block("inode_seg0.bin", parent_inode_num, inode_buffer);
        }
        
        // Now read the directory block
        uint8_t dir_block[BLOCK_SIZE];
        read_block("data_seg0.bin", dir_block_num, dir_block);
        dir_entry *entries = (dir_entry *)dir_block;
        
        // Look for a free entry slot
        for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
            if (entries[i].inode_number == 0) {
                // Found a free slot
                entries[i].inode_number = inode_num;
                strncpy(entries[i].name, file_name, MAX_NAME_LEN - 1);
                entries[i].name[MAX_NAME_LEN - 1] = '\0';
                
                // Write updated directory block
                write_block("data_seg0.bin", dir_block_num, dir_block);
                found_free_slot = 1;
                printf("Added entry at index %d in directory block %d\n", i, dir_block_num);
                break;
            }
        }
    }
    
    if (!found_free_slot) {
        printf("Error: Directory is full, cannot add more entries!\n");
    } else {
        printf("Entry added to directory successfully\n");
    }
}

void add_file(const char *path, const char *src_filename) {
    // Create a copy of the path for manipulation
    char *path_copy = strdup(path);
    if (!path_copy) {
        perror("Failed to allocate memory");
        return;
    }

    // Find the last slash to separate directory from filename
    char *file_name = strrchr(path_copy, '/');
    if (!file_name) {
        printf("Invalid path: %s (no slash found)\n", path);
        free(path_copy);
        return;
    }

    // Handle case where filename is empty (path ends with slash)
    file_name++; // Move past the '/'
    if (strlen(file_name) == 0) {
        printf("Invalid path: %s (empty filename)\n", path);
        free(path_copy);
        return;
    }

    // Create parent path
    char parent_path[MAX_NAME_LEN];
    int parent_length = file_name - path_copy - 1;
    if (parent_length > 0) {
        strncpy(parent_path, path_copy, parent_length);
        parent_path[parent_length] = '\0';
    } else {
        // Root directory
        strcpy(parent_path, "/");
    }

    // Look up parent directory
    int parent_inode_num = lookup_path(parent_path, 1);
    if (parent_inode_num < 0) {
        printf("Parent directory '%s' not found!\n", parent_path);
        free(path_copy);
        return;
    }

    // Allocate new inode for the file
    int new_inode_num = allocate_inode();
    if (new_inode_num < 0) {
        printf("No free inodes available!\n");
        free(path_copy);
        return;
    }

    printf("Successfully allocated inode %d for file %s\n", new_inode_num, file_name);

    // Initialize the new inode
    inode_field new_inode = {0};
    new_inode.type = TYPE_FILE;
    new_inode.size = 0;

    // Open source file
    FILE *src = fopen(src_filename, "rb");
    if (!src) {
        perror("Error opening source file");
        free(path_copy);
        return;
    }

    // Copy data from source file to our filesystem
    uint8_t buffer[BLOCK_SIZE];
    int blocks_written = 0;
    int indirect_allocated = 0;
    uint32_t indirect_block_num = 0;

    while (1) {
        size_t bytes_read = fread(buffer, 1, BLOCK_SIZE, src);
        if (bytes_read == 0) break;

        int data_block_num = allocate_data_block();
        if (data_block_num < 0) {
            printf("No free data blocks available!\n");
            // Clean up the inode we allocated but couldn't use fully
            uint8_t inode_bitmap[BLOCK_SIZE];
            read_block("inode_seg0.bin", 0, inode_bitmap);
            bitmap_clear(inode_bitmap, new_inode_num);
            write_block("inode_seg0.bin", 0, inode_bitmap);
            break;
        }

        printf("Allocated data block %d for file content\n", data_block_num);
        write_block("data_seg0.bin", data_block_num, buffer);

        if (blocks_written < 12) {
            new_inode.direct_pointers[blocks_written] = data_block_num;
        } else {
            if (!indirect_allocated) {
                // Allocate indirect block
                indirect_block_num = allocate_data_block();
                if (indirect_block_num < 0) {
                    printf("No free block for indirect pointer table!\n");
                    break;
                }
                new_inode.indirect_pointers = indirect_block_num;

                uint8_t empty[BLOCK_SIZE] = {0};
                write_block("data_seg0.bin", indirect_block_num, empty);
                indirect_allocated = 1;
            }

            uint8_t indirect_block[BLOCK_SIZE];
            read_block("data_seg0.bin", indirect_block_num, indirect_block);
            uint32_t *indirect_entries = (uint32_t *)indirect_block;

            indirect_entries[blocks_written - 12] = data_block_num;

            write_block("data_seg0.bin", indirect_block_num, indirect_block);
        }

        new_inode.size += bytes_read;
        blocks_written++;
    }

    fclose(src);

    // Write the new inode
    write_block("inode_seg0.bin", new_inode_num, (uint8_t *)&new_inode);
    printf("Written inode %d to disk with size %u bytes\n", new_inode_num, new_inode.size);
    
    // Add entry to parent directory
    add_entry_to_directory(parent_inode_num, file_name, new_inode_num);

    printf("File '%s' added successfully with inode %d!\n", file_name, new_inode_num);
    free(path_copy);
}

void extract_file(const char *path) {
    // Look up the inode number for the path
    int inode_number = lookup_path(path,0);
    if (inode_number < 0) {
        printf("File '%s' not found.\n", path);
        return;
    }

    // Read the inode
    uint8_t inode_buffer[BLOCK_SIZE];
    read_block("inode_seg0.bin", inode_number, inode_buffer);
    inode_field *file_inode = (inode_field *)inode_buffer;

    // Verify it's a file
    if (file_inode->type != TYPE_FILE) {
        printf("Error: '%s' is not a file!\n", path);
        return;
    }

    // Create output file (or use stdout)
    // For now, let's create a file with the same name as the last component of the path
    char *filename = strrchr(path, '/');
    if (!filename) {
        filename = (char *)path;  // No '/' found, entire path is the filename
    } else {
        filename++;  // Skip the '/'
    }
    
    FILE *outfile = fopen(filename, "wb");
    if (!outfile) {
        perror("Error creating output file");
        return;
    }

    // Read data from direct pointers
    uint8_t buffer[BLOCK_SIZE];
    uint32_t bytes_remaining = file_inode->size;
    
    // Process direct pointers
    for (int i = 0; i < 12 && bytes_remaining > 0; i++) {
        if (file_inode->direct_pointers[i] == 0) continue;
        
        read_block("data_seg0.bin", file_inode->direct_pointers[i], buffer);
        
        // Write either the full block or just the remaining bytes
        uint32_t bytes_to_write = (bytes_remaining < BLOCK_SIZE) ? bytes_remaining : BLOCK_SIZE;
        fwrite(buffer, 1, bytes_to_write, outfile);
        bytes_remaining -= bytes_to_write;
    }
    
    // Process indirect pointer if necessary
    if (bytes_remaining > 0 && file_inode->indirect_pointers != 0) {
        uint8_t indirect_block[BLOCK_SIZE];
        read_block("data_seg0.bin", file_inode->indirect_pointers, indirect_block);
        uint32_t *indirect_entries = (uint32_t *)indirect_block;
        
        for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t) && bytes_remaining > 0; i++) {
            if (indirect_entries[i] == 0) continue;
            
            read_block("data_seg0.bin", indirect_entries[i], buffer);
            
            uint32_t bytes_to_write = (bytes_remaining < BLOCK_SIZE) ? bytes_remaining : BLOCK_SIZE;
            fwrite(buffer, 1, bytes_to_write, outfile);
            bytes_remaining -= bytes_to_write;
        }
    }
    
    if (bytes_remaining > 0) {
        printf("Warning: file seems truncated, missing blocks.\n");
    }
    
    fclose(outfile);
    printf("File '%s' extracted successfully.\n", filename);
}

void remove_path(const char *path) {
    if (strcmp(path, "/") == 0) {
        printf("Error: Cannot remove root directory '/'.\n");
        return;
    }

    int inode_number = lookup_path(path,0);
    if (inode_number < 0) {
        printf("Path '%s' not found.\n", path);
        return;
    }

    uint8_t inode_buffer[BLOCK_SIZE];
    read_block("inode_seg0.bin", inode_number, inode_buffer);
    inode_field *target_inode = (inode_field *)inode_buffer;

    char *parent_path = strdup(path);
    char *name = strrchr(parent_path, '/');
    if (!name) {
        printf("Cannot remove root directory.\n");
        free(parent_path);
        return;
    }
    *name = '\0';
    name++;

    if (strlen(parent_path) == 0) strcpy(parent_path, "/");

    int parent_inode_num = lookup_path(parent_path,0);
    if (parent_inode_num < 0) {
        printf("Parent directory not found!\n");
        free(parent_path);
        return;
    }

    // If it's a directory, recursively delete its contents
    if (target_inode->type == TYPE_DIR) {
        for (int dp = 0; dp < 12; dp++) {
            if (target_inode->direct_pointers[dp] == 0) continue;

            uint8_t dir_block[BLOCK_SIZE];
            read_block("data_seg0.bin", target_inode->direct_pointers[dp], dir_block);
            dir_entry *entries = (dir_entry *)dir_block;

            for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
                if (entries[i].inode_number != 0) {
                    // Skip "." and ".."
                    if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) {
                        continue;
                    }

                    char child_path[MAX_NAME_LEN * 2];
                    snprintf(child_path, sizeof(child_path), "%s/%s", path, entries[i].name);

                    remove_path(child_path);
                }
            }
        }

        // (Optional) Handle indirect pointers if the directory is huge
    }

    // Read bitmaps
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t data_bitmap[BLOCK_SIZE];
    read_block("inode_seg0.bin", 0, inode_bitmap);
    read_block("data_seg0.bin", 0, data_bitmap);

    // Free direct blocks
    for (int i = 0; i < 12; i++) {
        if (target_inode->direct_pointers[i] != 0) {
            int block_num = target_inode->direct_pointers[i];
            bitmap_clear(data_bitmap, block_num);
        }
    }

    // Free indirect blocks if any
    if (target_inode->indirect_pointers != 0) {
        uint8_t indirect_block[BLOCK_SIZE];
        read_block("data_seg0.bin", target_inode->indirect_pointers, indirect_block);
        uint32_t *indirect_entries = (uint32_t *)indirect_block;

        for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t); i++) {
            if (indirect_entries[i] != 0) {
                bitmap_clear(data_bitmap, indirect_entries[i]);
            }
        }

        // Free the indirect block itself
        bitmap_clear(data_bitmap, target_inode->indirect_pointers);
    }

    // Free inode itself
    bitmap_clear(inode_bitmap, inode_number);

    // Clear inode block
    uint8_t empty_block[BLOCK_SIZE] = {0};
    write_block("inode_seg0.bin", inode_number, empty_block);

    // Write updated bitmaps
    write_block("inode_seg0.bin", 0, inode_bitmap);
    write_block("data_seg0.bin", 0, data_bitmap);

    // Remove entry from parent directory
    uint8_t parent_inode_buffer[BLOCK_SIZE];
    read_block("inode_seg0.bin", parent_inode_num, parent_inode_buffer);
    inode_field *parent_inode = (inode_field *)parent_inode_buffer;

    for (int dp = 0; dp < 12; dp++) {
        if (parent_inode->direct_pointers[dp] == 0) continue;

        uint8_t dir_block[BLOCK_SIZE];
        read_block("data_seg0.bin", parent_inode->direct_pointers[dp], dir_block);
        dir_entry *entries = (dir_entry *)dir_block;

        for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
            if (entries[i].inode_number == inode_number) {
                entries[i].inode_number = 0;
                entries[i].name[0] = '\0';
                write_block("data_seg0.bin", parent_inode->direct_pointers[dp], dir_block);
                break;
            }
        }
    }

    free(parent_path);
    printf("Successfully removed '%s'\n", path);
}


void debug_info() {
    printf("ExFS2 Debug Information\n");
    printf("=======================\n\n");

    // Read inode bitmap
    uint8_t inode_bitmap[BLOCK_SIZE];
    read_block("inode_seg0.bin", 0, inode_bitmap);

    // Count used inodes
    int used_inodes = 0;
    for (int i = 0; i < BLOCK_SIZE; i++) {
        for (int bit = 0; bit < 8; bit++) {
            if (inode_bitmap[i] & (1 << bit)) {
                used_inodes++;
            }
        }
    }

    printf("Inode Information:\n");
    printf("  Total inodes: %d\n", BLOCK_SIZE * 8);
    printf("  Used inodes: %d\n", used_inodes);
    printf("  Free inodes: %d\n\n", BLOCK_SIZE * 8 - used_inodes);

    // Read data bitmap
    uint8_t data_bitmap[BLOCK_SIZE];
    read_block("data_seg0.bin", 0, data_bitmap);

    // Count used data blocks
    int used_blocks = 0;
    for (int i = 0; i < BLOCK_SIZE; i++) {
        for (int bit = 0; bit < 8; bit++) {
            if (data_bitmap[i] & (1 << bit)) {
                used_blocks++;
            }
        }
    }

    printf("Data Block Information:\n");
    printf("  Total blocks: %d\n", TOTAL_BLOCKS);
    printf("  Used blocks: %d\n", used_blocks);
    printf("  Free blocks: %d\n\n", TOTAL_BLOCKS - used_blocks);

    // Read all inodes into memory once
    uint8_t *all_inodes = malloc(BLOCK_SIZE * TOTAL_BLOCKS);
    if (!all_inodes) {
        perror("malloc failed for inodes");
        exit(1);
    }
    FILE *fp = fopen("inode_seg0.bin", "rb");
    if (!fp) {
        perror("Failed to open inode segment");
        free(all_inodes);
        exit(1);
    }
    fread(all_inodes, 1, BLOCK_SIZE * TOTAL_BLOCKS, fp);
    fclose(fp);

    printf("Inode Details:\n");
    for (int i = 1; i < BLOCK_SIZE * 8; i++) { // Skip inode 0 (reserved)
        int byte_index = i / 8;
        int bit_index = i % 8;

        if (inode_bitmap[byte_index] & (1 << bit_index)) {
            inode_field *inode = (inode_field *)(all_inodes + i * BLOCK_SIZE);

            printf("  Inode %d:\n", i);
            printf("    Type: %s\n", inode->type == TYPE_FILE ? "File" : "Directory");
            printf("    Size: %u bytes\n", inode->size);

            printf("    Direct pointers: ");
            for (int dp = 0; dp < 12; dp++) {
                if (inode->direct_pointers[dp] != 0) {
                    printf("%u ", inode->direct_pointers[dp]);
                }
            }
            printf("\n");

            if (inode->indirect_pointers != 0) {
                printf("    Indirect pointer: %u\n", inode->indirect_pointers);

                uint8_t indirect_block[BLOCK_SIZE];
                read_block("data_seg0.bin", inode->indirect_pointers, indirect_block);
                uint32_t *indirect_entries = (uint32_t *)indirect_block;

                printf("    Indirect blocks: ");
                for (int ip = 0; ip < BLOCK_SIZE / sizeof(uint32_t); ip++) {
                    if (indirect_entries[ip] != 0) {
                        printf("%u ", indirect_entries[ip]);
                    }
                }
                printf("\n");
            }

            printf("\n"); // Extra newline for readability
        }
    }

    // Cleanup
    free(all_inodes);
}


int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s [-F] [-a path -f src_file] [-e path] [-r path] [-l path] [-D]\n", argv[0]);
        return 1;
    }

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-F") == 0) {
            format_filesystem();

            uint8_t buffer[BLOCK_SIZE];
            read_block("inode_seg0.bin", 1, buffer);

            inode_field *root_inode = (inode_field *)buffer;
            printf("After reading back: root inode type = %d, size = %u, first direct = %u\n",
                   root_inode->type, root_inode->size, root_inode->direct_pointers[0]);

            i++;
        } 
        else if (strcmp(argv[i], "-a") == 0) {
            if (i + 3 < argc && strcmp(argv[i + 2], "-f") == 0) {
                printf("Add file to path: %s with source file: %s\n", argv[i + 1], argv[i + 3]);
                add_file(argv[i+1], argv[i+3]);
                i += 4;
            } else {
                printf("Error: -a requires a path and a source file (-f srcfile)\n");
                return 1;
            }
        } 
        else if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 < argc) {
                printf("Extract file: %s\n", argv[i + 1]);
                extract_file(argv[i + 1]);
                i += 2;
            } else {
                printf("Error: -e requires a path\n");
                return 1;
            }
        } 
        else if (strcmp(argv[i], "-r") == 0) {
            if (i + 1 < argc) {
                printf("Remove path: %s\n", argv[i + 1]);
                remove_path(argv[i+1]);
                i += 2;
            } else {
                printf("Error: -r requires a path\n");
                return 1;
            }
        } 
        else if (strcmp(argv[i], "-l") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                list_directory(argv[i + 1]);
                i += 2;
            } else {
                list_directory("/");
                i++;
            }
        } 
        else if (strcmp(argv[i], "-D") == 0) {
            printf("Debug info requested\n");
            debug_info();
            i++;
        } 
        else {
            printf("Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    return 0;
}
