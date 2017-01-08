/*
 * Implementation of a Unix-like file system.
*/
#ifndef FS_INCLUDED
#define FS_INCLUDED

#include "block.h"
//number of sectors 
#define FS_SIZE 2048

void fs_init( void);
int fs_mkfs( void);
int fs_open( char *fileName, int flags);
int fs_close( int fd);
int fs_read( int fd, char *buf, int count);
int fs_write( int fd, char *buf, int count);
int fs_lseek( int fd, int offset);
int fs_mkdir( char *fileName);
int fs_rmdir( char *fileName);
int fs_cd( char *dirName);
int fs_link( char *old_fileName, char *new_fileName);
int fs_unlink( char *fileName);
int fs_stat( char *fileName, fileStat *buf);

#define MAX_FILE_NAME 32
#define MAX_PATH_NAME 256  // This is the maximum supported "full" path len, eg: /foo/bar/test.txt, rather than the maximum individual filename len.


//this unix-like file sys is write_through

int fs_cd_inode_id(int dir_id);


#define MAX_FILE_COUNT (BLOCK_SIZE*8)

#if (BLOCK_SIZE*DIRECT_BLOCK+BLOCK_SIZE*BLOCK_SIZE/16) < (DATA_BLOCK_NUMBER*BLOCK_SIZE)
	#define MAX_FILE_SIZE (BLOCK_SIZE*DIRECT_BLOCK+BLOCK_SIZE*BLOCK_SIZE/16)
#else
	#define MAX_FILE_SIZE (DATA_BLOCK_NUMBER*BLOCK_SIZE)
#endif

#define MAX_FILE_ONE_DIR (DIR_ENTRY_PER_BLOCK*DIRECT_BLOCK+DIR_ENTRY_PER_BLOCK*BLOCK_SIZE/16)

#define INODE_BITMAP 0
#define DBLOCK_BITMAP 1

#define MY_DIRECTORY 0
#define REAL_FILE 1


// -- super block -----------------------------------
#define SUPER_BLOCK 1
#define SUPER_BLOCK_BACKUP (FS_SIZE-1)
#define INODE_BLOCK_NUMBER (MAX_FILE_COUNT/INODE_PER_BLOCK)
#define DATA_BLOCK_NUMBER (FS_SIZE-5-INODE_BLOCK_NUMBER)



#define SB_PADDING (BLOCK_SIZE-18)
#define MY_MAGIC 4008208820
typedef struct __attribute__ ((__packed__))
{
	uint16_t file_sys_size;
	uint16_t inode_bitmap_place;
	uint16_t inode_start;
	uint16_t inode_count;
	uint32_t magic_num;
	uint16_t dblock_bitmap_place;
	uint16_t dblock_start;
	uint16_t dblock_count;

	char _padding[SB_PADDING];

}super_b;

// -- inode -----------------------------------
#define DIRECT_BLOCK 11
#define INODE_PADDING 0
//total size: 32 bytes
// #define INODE_SIZE 32
#define INODE_PER_BLOCK (BLOCK_SIZE/32)

#define MAX_BLOCKS_INDEX_IN_INODE (DIRECT_BLOCK+BLOCK_SIZE/16)
typedef struct __attribute__ ((__packed__))
{
	uint32_t size;//in bytes
	uint16_t type;//0 for dir, 1 for file
	uint16_t link_count;
	uint16_t blocks[DIRECT_BLOCK+1];//start from 0 as data block index
	//char _padding[INODE_PADDING];
}inode;
// -- dir_entry -----------------------------------
//id for No. in inode table
#define ROOT_DIR_ID 0
#define DIR_ENTRY_PADDING (64-2-MAX_FILE_NAME-1)
//total size: 64 bytes
#define DIR_ENTRY_PER_BLOCK (BLOCK_SIZE/64)
typedef struct __attribute__ ((__packed__))
{
	uint16_t inode_id;
	char file_name[MAX_FILE_NAME+1];

	char _padding[DIR_ENTRY_PADDING];
}dir_entry;


// --- above is on-disk   ---

// --- below is on-memory ---

#define MAX_OPEN_FILE_NUM 256

typedef struct 
{
	bool_t is_using;
	uint32_t cursor;//in bytes
	uint16_t inode_id;
	uint16_t mode;//(FS_O_RDONLY, FS_O_WRONLY, FS_ORDWR)
}file_desc;

#endif
