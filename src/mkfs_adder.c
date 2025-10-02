#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <libgen.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t  type;
    char     name[58];
    uint8_t  checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

// CRC32 functions
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

void usage(const char* prog) {
    fprintf(stderr, "Usage: %s --input <input.img> --output <output.img> --file <file>\n", prog);
    exit(1);
}

int find_free_bit(uint8_t* bitmap, int max_bits) {
    for (int i = 0; i < max_bits; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            return i;
        }
    }
    return -1;
}

void set_bit(uint8_t* bitmap, int bit) {
    int byte_idx = bit / 8;
    int bit_idx = bit % 8;
    bitmap[byte_idx] |= (1 << bit_idx);
}

int main(int argc, char* argv[]) {
    crc32_init();
    
    // Parse CLI parameters
    const char* input_file = NULL;
    const char* output_file = NULL;
    const char* file_to_add = NULL;
    
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            usage(argv[0]);
        }
        
        if (strcmp(argv[i], "--input") == 0) {
            input_file = argv[i + 1];
        } else if (strcmp(argv[i], "--output") == 0) {
            output_file = argv[i + 1];
        } else if (strcmp(argv[i], "--file") == 0) {
            file_to_add = argv[i + 1];
        } else {
            usage(argv[0]);
        }
    }
    
    if (!input_file || !output_file || !file_to_add) {
        usage(argv[0]);
    }
    
    // Open input image
    FILE* input_fp = fopen(input_file, "rb");
    if (!input_fp) {
        perror("fopen input");
        return 1;
    }
    
    // Read superblock with error check
    uint8_t sb_block[BS];
    if (fread(sb_block, BS, 1, input_fp) != 1) {
        perror("Error reading superblock");
        fclose(input_fp);
        return 1;
    }
    superblock_t* sb = (superblock_t*)sb_block;
    
    if (sb->magic != 0x4D565346) {
        fprintf(stderr, "Invalid magic number\n");
        fclose(input_fp);
        return 1;
    }
    
    // Read bitmaps with error checks
    uint8_t inode_bitmap[BS], data_bitmap[BS];
    fseek(input_fp, sb->inode_bitmap_start * BS, SEEK_SET);
    if (fread(inode_bitmap, BS, 1, input_fp) != 1) {
        perror("Error reading inode bitmap");
        fclose(input_fp);
        return 1;
    }
    
    fseek(input_fp, sb->data_bitmap_start * BS, SEEK_SET);
    if (fread(data_bitmap, BS, 1, input_fp) != 1) {
        perror("Error reading data bitmap");
        fclose(input_fp);
        return 1;
    }
    
    // Read inode table with error check
    size_t inode_table_size = sb->inode_table_blocks * BS;
    uint8_t* inode_table = malloc(inode_table_size);
    fseek(input_fp, sb->inode_table_start * BS, SEEK_SET);
    if (fread(inode_table, inode_table_size, 1, input_fp) != 1) {
        perror("Error reading inode table");
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Open file to add
    FILE* file_fp = fopen(file_to_add, "rb");
    if (!file_fp) {
        perror("fopen file to add");
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Get file size
    fseek(file_fp, 0, SEEK_END);
    size_t file_size = ftell(file_fp);
    fseek(file_fp, 0, SEEK_SET);
    
    // Calculate blocks needed
    int blocks_needed = (file_size + BS - 1) / BS;
    if (blocks_needed > DIRECT_MAX) {
        fprintf(stderr, "File too large (needs %d blocks, max %d)\n", blocks_needed, DIRECT_MAX);
        fclose(file_fp);
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Find free inode
    int free_inode = find_free_bit(inode_bitmap, sb->inode_count);
    if (free_inode == -1) {
        fprintf(stderr, "No free inodes\n");
        fclose(file_fp);
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Find free data blocks
    int free_blocks[DIRECT_MAX];
    int found_blocks = 0;
    for (int i = 0; i < (int)sb->data_region_blocks && found_blocks < blocks_needed; i++) {
        if (!(data_bitmap[i/8] & (1 << (i%8)))) {
            free_blocks[found_blocks++] = i;
        }
    }
    
    if (found_blocks < blocks_needed) {
        fprintf(stderr, "Not enough free data blocks\n");
        fclose(file_fp);
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Create new inode
    inode_t new_inode = {0};
    new_inode.mode = 0100000; // regular file (octal)
    new_inode.links = 1;
    new_inode.uid = 0;
    new_inode.gid = 0;
    new_inode.size_bytes = file_size;
    new_inode.atime = new_inode.mtime = new_inode.ctime = time(NULL);
    for (int i = 0; i < blocks_needed; i++) {
        new_inode.direct[i] = sb->data_region_start + free_blocks[i];
    }
    for (int i = blocks_needed; i < 12; i++) {
        new_inode.direct[i] = 0;
    }
    new_inode.proj_id = 4;
    inode_crc_finalize(&new_inode);
    
    // Update bitmaps
    set_bit(inode_bitmap, free_inode);
    for (int i = 0; i < blocks_needed; i++) {
        set_bit(data_bitmap, free_blocks[i]);
    }
    
    // Add inode to table
    memcpy(inode_table + free_inode * INODE_SIZE, &new_inode, sizeof(new_inode));
    
    // Read root directory with error check
    inode_t* root_inode = (inode_t*)(inode_table + (ROOT_INO - 1) * INODE_SIZE);
    uint8_t root_dir_block[BS];
    fseek(input_fp, root_inode->direct[0] * BS, SEEK_SET);
    if (fread(root_dir_block, BS, 1, input_fp) != 1) {
        perror("Error reading root directory");
        fclose(file_fp);
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Find free directory entry slot
    dirent64_t* entries = (dirent64_t*)root_dir_block;
    int max_entries = BS / sizeof(dirent64_t);
    int free_entry = -1;
    for (int i = 0; i < max_entries; i++) {
        if (entries[i].inode_no == 0) {
            free_entry = i;
            break;
        }
    }
    
    if (free_entry == -1) {
        fprintf(stderr, "Root directory full\n");
        fclose(file_fp);
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Create directory entry
    dirent64_t new_entry = {0};
    new_entry.inode_no = free_inode + 1; // 1-indexed
    new_entry.type = 1; // file
    char* filename = basename((char*)file_to_add);
    strncpy(new_entry.name, filename, sizeof(new_entry.name) - 1);
    dirent_checksum_finalize(&new_entry);
    
    memcpy(&entries[free_entry], &new_entry, sizeof(new_entry));
    
    // Update root inode links
    root_inode->links++;
    inode_crc_finalize(root_inode);
    
    // Create output file
    FILE* output_fp = fopen(output_file, "wb");
    if (!output_fp) {
        perror("fopen output");
        fclose(file_fp);
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Copy input to output with error checks
    fseek(input_fp, 0, SEEK_SET);
    uint8_t buffer[BS];
    for (uint64_t i = 0; i < sb->total_blocks; i++) {
        if (fread(buffer, BS, 1, input_fp) != 1) {
            perror("Error copying input file");
            fclose(file_fp);
            free(inode_table);
            fclose(input_fp);
            fclose(output_fp);
            return 1;
        }
        fwrite(buffer, BS, 1, output_fp);
    }
    
    fclose(input_fp);
    
    // Write updated structures
    fseek(output_fp, 0, SEEK_SET);
    fwrite(sb_block, BS, 1, output_fp);
    
    fseek(output_fp, sb->inode_bitmap_start * BS, SEEK_SET);
    fwrite(inode_bitmap, BS, 1, output_fp);
    
    fseek(output_fp, sb->data_bitmap_start * BS, SEEK_SET);
    fwrite(data_bitmap, BS, 1, output_fp);
    
    fseek(output_fp, sb->inode_table_start * BS, SEEK_SET);
    fwrite(inode_table, inode_table_size, 1, output_fp);
    
    fseek(output_fp, root_inode->direct[0] * BS, SEEK_SET);
    fwrite(root_dir_block, BS, 1, output_fp);
    
    // Write file data with error checks
    for (int i = 0; i < blocks_needed; i++) {
        fseek(output_fp, new_inode.direct[i] * BS, SEEK_SET);
        size_t to_read = (i == blocks_needed - 1) ? (file_size % BS) : BS;
        if (to_read == 0) to_read = BS;
        
        memset(buffer, 0, BS);
        if (fread(buffer, to_read, 1, file_fp) != 1 && to_read > 0) {
            perror("Error reading file data");
            fclose(file_fp);
            fclose(output_fp);
            free(inode_table);
            return 1;
        }
        fwrite(buffer, BS, 1, output_fp);
    }
    
    fclose(file_fp);
    fclose(output_fp);
    free(inode_table);
    
    printf("File added successfully\n");
    return 0;
}

