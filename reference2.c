// exfs2_full.c - Complete, student-friendly ExFS2 implementation with path parsing and deletion

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK_SIZE 4096
#define SEGMENT_SIZE (1024 * 1024)
#define MAX_BLOCKS ((SEGMENT_SIZE - 512) / BLOCK_SIZE)
#define MAX_NAME_LEN 64
#define MAX_FILES 128
#define MAX_INODES 128
#define MAX_PATH_PARTS 10

// --- Data segment structure ---
typedef struct {
    uint8_t free_block_bitmap[512];
    char blocks[MAX_BLOCKS][BLOCK_SIZE];
} DataSegment;

// --- Inode structure ---
typedef struct {
    int is_used;
    int is_directory;
    int file_size;
    int direct_blocks[10];
} Inode;

// --- Directory Entry ---
typedef struct {
    char name[MAX_NAME_LEN];
    int inode_number;
} DirEntry;

// --- Inode segment structure ---
typedef struct {
    uint8_t inode_bitmap[MAX_INODES];
    Inode inodes[MAX_INODES];
} InodeSegment;

// --- Global filesystem ---
InodeSegment inode_segment;
DataSegment data_segment;

// --- Utility Functions ---
int find_free_inode() {
    for (int i = 0; i < MAX_INODES; i++) {
        if (!inode_segment.inode_bitmap[i]) {
            inode_segment.inode_bitmap[i] = 1;
            inode_segment.inodes[i].is_used = 1;
            return i;
        }
    }
    return -1;
}

int find_free_block() {
    for (int i = 0; i < MAX_BLOCKS; i++) {
        int byte = i / 8;
        int bit = i % 8;
        if (!(data_segment.free_block_bitmap[byte] & (1 << bit))) {
            data_segment.free_block_bitmap[byte] |= (1 << bit);
            return i;
        }
    }
    return -1;
}

void save_segments(const char* inode_file, const char* data_file) {
    FILE* f1 = fopen(inode_file, "wb");
    FILE* f2 = fopen(data_file, "wb");
    fwrite(&inode_segment, sizeof(InodeSegment), 1, f1);
    fwrite(&data_segment, sizeof(DataSegment), 1, f2);
    fclose(f1);
    fclose(f2);
}

void load_segments(const char* inode_file, const char* data_file) {
    FILE* f1 = fopen(inode_file, "rb");
    FILE* f2 = fopen(data_file, "rb");

    if (!f1 || !f2) {
        printf("Error: Segment files not found. Please run with -init first.\n");
        exit(1);
    }

    fread(&inode_segment, sizeof(InodeSegment), 1, f1);
    fread(&data_segment, sizeof(DataSegment), 1, f2);
    fclose(f1);
    fclose(f2);
}



void init_fs(const char* inode_file, const char* data_file) {
    memset(&inode_segment, 0, sizeof(InodeSegment));
    memset(&data_segment, 0, sizeof(DataSegment));

    int root_inode = find_free_inode();
    inode_segment.inodes[root_inode].is_directory = 1;
    inode_segment.inodes[root_inode].file_size = 0;
    int block = find_free_block();
    inode_segment.inodes[root_inode].direct_blocks[0] = block;

    save_segments(inode_file, data_file);
    printf("Filesystem initialized.\n");
}

int find_entry(Inode* dir, const char* name) {
    int block = dir->direct_blocks[0];
    DirEntry* entries = (DirEntry*) data_segment.blocks[block];
    int count = dir->file_size / sizeof(DirEntry);
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0)
            return entries[i].inode_number;
    }
    return -1;
}

int add_entry(Inode* dir, const char* name, int inode_num) {
    int block = dir->direct_blocks[0];
    DirEntry* entries = (DirEntry*) data_segment.blocks[block];
    int count = dir->file_size / sizeof(DirEntry);
    strcpy(entries[count].name, name);
    entries[count].inode_number = inode_num;
    dir->file_size += sizeof(DirEntry);
    return 0;
}

int traverse_path(const char* path, int create_missing, int* parent_inode, char* final_name) {
    char path_copy[256];
    strcpy(path_copy, path);
    char* parts[MAX_PATH_PARTS];
    int count = 0;
    char* token = strtok(path_copy, "/");
    while (token && count < MAX_PATH_PARTS) {
        parts[count++] = token;
        token = strtok(NULL, "/");
    }
    int curr_inode = 0;
    for (int i = 0; i < count - 1; i++) {
        int next_inode = find_entry(&inode_segment.inodes[curr_inode], parts[i]);
        if (next_inode == -1) {
            if (!create_missing) return -1;
            int ni = find_free_inode();
            inode_segment.inodes[ni].is_directory = 1;
            inode_segment.inodes[ni].file_size = 0;
            int blk = find_free_block();
            inode_segment.inodes[ni].direct_blocks[0] = blk;
            add_entry(&inode_segment.inodes[curr_inode], parts[i], ni);
            curr_inode = ni;
        } else {
            curr_inode = next_inode;
        }
    }
    if (parent_inode) *parent_inode = curr_inode;
    if (final_name) strcpy(final_name, parts[count - 1]);
    return find_entry(&inode_segment.inodes[curr_inode], parts[count - 1]);
}

int add_file(const char* virtual_path, const char* local_path) {
    FILE* src = fopen(local_path, "rb");
    if (!src) { perror("open source"); return -1; }

    fseek(src, 0, SEEK_END);
    int fsize = ftell(src);
    rewind(src);

    int inode_index = find_free_inode();
    Inode* inode = &inode_segment.inodes[inode_index];
    inode->file_size = fsize;
    inode->is_directory = 0;

    for (int i = 0; i < 10 && fsize > 0; i++) {
        int block = find_free_block();
        inode->direct_blocks[i] = block;
        fread(data_segment.blocks[block], 1, BLOCK_SIZE, src);
        fsize -= BLOCK_SIZE;
    }
    fclose(src);

    int parent;
    char name[MAX_NAME_LEN];
    traverse_path(virtual_path, 1, &parent, name);
    add_entry(&inode_segment.inodes[parent], name, inode_index);
    return 0;
}

void remove_entry(Inode* dir, const char* name) {
    int block = dir->direct_blocks[0];
    DirEntry* entries = (DirEntry*) data_segment.blocks[block];
    int count = dir->file_size / sizeof(DirEntry);
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            for (int j = i; j < count - 1; j++)
                entries[j] = entries[j + 1];
            dir->file_size -= sizeof(DirEntry);
            return;
        }
    }
}

void remove_path(const char* path) {
    int parent;
    char name[MAX_NAME_LEN];
    int inode_index = traverse_path(path, 0, &parent, name);
    if (inode_index == -1) { printf("Path not found\n"); return; }
    Inode* node = &inode_segment.inodes[inode_index];
    if (node->is_directory) {
        int block = node->direct_blocks[0];
        DirEntry* entries = (DirEntry*) data_segment.blocks[block];
        int count = node->file_size / sizeof(DirEntry);
        for (int i = 0; i < count; i++) {
            char child_path[256];
            sprintf(child_path, "%s/%s", path, entries[i].name);
            remove_path(child_path);
        }
    }
    inode_segment.inode_bitmap[inode_index] = 0;
    inode_segment.inodes[inode_index].is_used = 0;
    remove_entry(&inode_segment.inodes[parent], name);
    printf("Removed: %s\n", path);
}

void extract_file(const char* path) {
    int inode_index = traverse_path(path, 0, NULL, NULL);
    if (inode_index == -1) { printf("File not found\n"); return; }
    Inode* file = &inode_segment.inodes[inode_index];
    int size = file->file_size;
    for (int i = 0; i < 10 && size > 0; i++) {
        int b = file->direct_blocks[i];
        int s = size > BLOCK_SIZE ? BLOCK_SIZE : size;
        fwrite(data_segment.blocks[b], 1, s, stdout);
        size -= s;
    }
}

void list_fs(int inode_index, int depth) {
    Inode* node = &inode_segment.inodes[inode_index];
    if (!node->is_directory) return;
    int block = node->direct_blocks[0];
    DirEntry* entries = (DirEntry*) data_segment.blocks[block];
    int count = node->file_size / sizeof(DirEntry);
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < depth; j++) printf("  ");
        printf("%s\n", entries[i].name);
        if (inode_segment.inodes[entries[i].inode_number].is_directory) {
            list_fs(entries[i].inode_number, depth + 1);
        }
    }
}

// --- Main driver ---
int main(int argc, char* argv[]) {
    const char* inode_file = "inode.seg";
    const char* data_file = "data.seg";
    if (argc < 2) {
        printf("Usage: ./exfs2 -init|-add|-list|-extract|-remove [args]\n");
        return 1;
    }
    if (strcmp(argv[1], "-init") == 0) {
        init_fs(inode_file, data_file);
    } else if (strcmp(argv[1], "-add") == 0 && argc == 4) {
        load_segments(inode_file, data_file);
        add_file(argv[2], argv[3]);
        save_segments(inode_file, data_file);
    } else if (strcmp(argv[1], "-list") == 0) {
        load_segments(inode_file, data_file);
        list_fs(0, 0);
    } else if (strcmp(argv[1], "-extract") == 0 && argc == 3) {
        load_segments(inode_file, data_file);
        extract_file(argv[2]);
    } else if (strcmp(argv[1], "-remove") == 0 && argc == 3) {
        load_segments(inode_file, data_file);
        remove_path(argv[2]);
        save_segments(inode_file, data_file);
    } else {
        printf("Invalid command or arguments.\n");
    }
    return 0;
}