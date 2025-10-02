#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

#define BS 4096u              
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0;

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

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
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
    fprintf(stderr, "Usage: %s --image <output.img> --size-kib <180-4096> --inodes <128-512>\n", prog);
    exit(1);
}

int main(int argc, char* argv[]) {
    crc32_init();
    
    // Parse CLI parameters
    const char* image_file = NULL;
    int size_kib = 0;
    int inode_count = 0;
    
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            usage(argv[0]);
        }
        
        if (strcmp(argv[i], "--image") == 0) {
            image_file = argv[i + 1];
        } else if (strcmp(argv[i], "--size-kib") == 0) {
            size_kib = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--inodes") == 0) {
            inode_count = atoi(argv[i + 1]);
        } else {
            usage(argv[0]);
        }
    }
    

    if (!image_file || size_kib < 180 || size_kib > 4096 || (size_kib % 4) != 0 ||
        inode_count < 128 || inode_count > 512) {
        usage(argv[0]);
    }
    

    uint64_t total_blocks = (uint64_t)size_kib * 1024 / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    
    uint64_t inode_bitmap_start = 1;
    uint64_t data_bitmap_start = 2;
    uint64_t inode_table_start = 3;
    uint64_t data_region_start = inode_table_start + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;
    
    if (data_region_blocks < 1) {
        fprintf(stderr, "Error: Not enough space for data region\n");
        return 1;
    }
    
    superblock_t sb = {0};
    sb.magic = 0x4D565346;
    sb.version = 1;
    sb.block_size = BS;
    sb.total_blocks = total_blocks;
    sb.inode_count = inode_count;
    sb.inode_bitmap_start = inode_bitmap_start;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = data_bitmap_start;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = inode_table_start;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_region_start = data_region_start;
    sb.data_region_blocks = data_region_blocks;
    sb.root_inode = ROOT_INO;
    sb.mtime_epoch = time(NULL);
    sb.flags = 0;
    
    // Create and open image file
    FILE* fp = fopen(image_file, "wb");
    if (!fp) {
        perror("fopen");
        return 1;
    }
    
    uint8_t block[BS] = {0};
    memcpy(block, &sb, sizeof(sb));
    superblock_crc_finalize((superblock_t*)block);
    fwrite(block, BS, 1, fp);
    
    // Write inode bitmap (root inode allocated)
    memset(block, 0, BS);
    block[0] = 0x01; 
    fwrite(block, BS, 1, fp);
    

    memset(block, 0, BS);
    block[0] = 0x01; 
    fwrite(block, BS, 1, fp);
    

    inode_t root_inode = {0};
    root_inode.mode = 040000;
    root_inode.links = 2; 
    root_inode.uid = 0;
    root_inode.gid = 0;
    root_inode.size_bytes = 2 * sizeof(dirent64_t);
    root_inode.atime = root_inode.mtime = root_inode.ctime = time(NULL);
    root_inode.direct[0] = data_region_start;
    for (int i = 1; i < 12; i++) root_inode.direct[i] = 0;
    root_inode.reserved_0 = root_inode.reserved_1 = root_inode.reserved_2 = 0;
    root_inode.proj_id = 4; 
    root_inode.uid16_gid16 = 0;
    root_inode.xattr_ptr = 0;
    inode_crc_finalize(&root_inode);
    
    // Write inode table
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        memset(block, 0, BS);
        if (i == 0) {
            memcpy(block, &root_inode, sizeof(root_inode));
        }
        fwrite(block, BS, 1, fp);
    }
    
    // Create root directory entries
    dirent64_t dot_entry = {0};
    dot_entry.inode_no = ROOT_INO;
    dot_entry.type = 2; 
    strcpy(dot_entry.name, ".");
    dirent_checksum_finalize(&dot_entry);
    
    dirent64_t dotdot_entry = {0};
    dotdot_entry.inode_no = ROOT_INO;
    dotdot_entry.type = 2; 
    strcpy(dotdot_entry.name, "..");
    dirent_checksum_finalize(&dotdot_entry);
    
    // Write root directory data block
    memset(block, 0, BS);
    memcpy(block, &dot_entry, sizeof(dot_entry));
    memcpy(block + sizeof(dirent64_t), &dotdot_entry, sizeof(dotdot_entry));
    fwrite(block, BS, 1, fp);
    
    // Write remaining data blocks (zeros)
    memset(block, 0, BS);
    for (uint64_t i = 1; i < data_region_blocks; i++) {
        fwrite(block, BS, 1, fp);
    }
    
    fclose(fp);
    printf("MiniVSFS image created: %s\n", image_file);
    return 0;
}

