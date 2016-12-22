/*
 * Implementation of a Unix-like file system.
*/
#include "util.h"
#include "common.h"
#include "block.h"
#include "fs.h"

#ifdef FAKE
#include <stdio.h>
#define ERROR_MSG(m) printf m;
#else
#define ERROR_MSG(m)
#endif

//static helper func
static int strcmp(char *s1,char *s2)
{

}
static int strlen_safe(char *src,int max_len)//max len without '\0'
{
	int i=0;
	while(*src++!='\0')
		i++;
	if(i>max_len)
		return max_len;
	else
		return i;
}
static int strcpy_safe(char *src,char *dest,int dest_max_len)//dest max len without final '\0' buffer
{
	bcopy(src,dest,strlen(src,dest_max_len));
	dest[dest_max_len]='\0';
}

//useful var-----------------------------
static char block_scratch[BLOCK_SIZE];//inode read/write/free use scratch

static char inode_bitmap_block_scratch[BLOCK_SIZE];
static char dblock_bitmap_block_scratch[BLOCK_SIZE];

static char super_block_scratch[BLOCK_SIZE];

static super_b * my_sb;

static file_des fd_table[MAX_OPEN_FILE_NUM];

static uint16_t pwd;//start from 0 as inode index

static uint16_t inode_bitmap_last=0;//start from 0
static uint16_t dblock_bitmap_last=0;//start from 0

//superblock write helper------------------
static void sb_write()
{
	block_write(SUPER_BLOCK,super_block_scratch);
	block_write(SUPER_BLOCK_BACKUP,super_block_scratch);
}
//bitmap helper-----------------------------

static int read_bitmap_block(int i_d,int index)// 0 for inode bitmap,1 for data bitmap
{
	char *bitmap_block_scratch;
	if(i_d)
		bitmap_block_scratch=dblock_bitmap_block_scratch;
	else
		bitmap_block_scratch=inode_bitmap_block_scratch;

	int nbyte=index/8;
	uint8_t the_byte=bitmap_block_scratch[nbyte];
	int mask_off=index%8;
	uint8_t mask=1<<mask_off;

	if(mask & the_byte)
		return 1;
	else
		return 0;
}

static void write_bitmap_block(int i_d,int index,int val) // 0 for inode bitmap,1 for data bitmap
{
	char *bitmap_block_scratch;
	if(i_d)
		bitmap_block_scratch=dblock_bitmap_block_scratch;
	else
		bitmap_block_scratch=inode_bitmap_block_scratch;

	int nbyte=index/8;
	uint8_t the_byte=bitmap_block_scratch[nbyte];
	int mask_off=index%8;
	uint8_t mask=1<<mask_off;
	the_byte = the_byte &(~mask);
	if(val)
		the_byte = the_byte | mask;

	bitmap_block_scratch[nbyte]=the_byte;
	
	if(i_d)
		block_write(sb->dblock_bitmap_place,bitmap_block_scratch);
	else
		block_write(sb->inode_bitmap_place,bitmap_block_scratch);
}
static int find_next_free(int i_d)//must alloc(write 1) after call this function
{
	int i;
	int res;
	if(i_d){
		i=(dblock_bitmap_last+1)%DATA_BLOCK_NUMBER;
		while(i!=dblock_bitmap_last){
			res=read_bitmap_block(1,i);
			if(res==0)
			{
				dblock_bitmap_last=i;
				return i;
			}
			i++;
			if(i==DATA_BLOCK_NUMBER)
				i=0;
		}
	}
	else{
		i=(inode_bitmap_last+1)%MAX_FILE_COUNT;
		while(i!=inode_bitmap_last){
			res=read_bitmap_block(0,i);
			if(res==0)
			{
				inode_bitmap_last=i;
				return i;
			}
			i++;
			if(i==MAX_FILE_COUNT)
				i=0;
		}
	}
	return -1;
}
//dblock alloc & free & read & write --------------------------
static int dblock_alloc(void)
{
	int search_res=-1;
	search_res=find_next_free(DBLOCK_BITMAP);
	if(search_res>=0)
	{
		write_bitmap_block(DBLOCK_BITMAP,search_res,1);
		my_sb->dblock_count++;
		sb_write();
		return search_res;
	}
	return -1;
}
static void dblock_free(int index)
{
	int temp=read_bitmap_block(DBLOCK_BITMAP,index);
	if (temp)
	{
		my_sb->dblock_count--;
		sb_write();
	}
	write_bitmap_block(DBLOCK_BITMAP,index,0);
}
//caller prepare space for whole data block
static void dblock_read(int index,char* block_buff)
{
	block_read(my_sb->dblock_start+index,block_buff);
}
//caller prepare space for whole data block
static void dblock_write(int index,char* block_buff)
{
	block_write(my_sb->dblock_start+index,block_buff);
}
//inode alloc & free & read & write & init helper ----------------------------------
static int inode_alloc(void)
{
	int search_res=-1;
	search_res=find_next_free(INODE_BITMAP);
	if(search_res>=0)
	{
		write_bitmap_block(INODE_BITMAP,search_res,1);
		my_sb->inode_count++;
		sb_write();
		return search_res;
	}
	return -1;
}

//caller prepare space for inode and check valid
static void inode_read(int index,char* inode_buff)
{
	char block_scratch[BLOCK_SIZE];
	block_read(my_sb->inode_start+(index/INODE_PER_BLOCK),block_scratch);
	bcopy(block_scratch+(index%INODE_PER_BLOCK)*sizeof(inode),inode_buff,sizeof(inode));
}
//caller prepare space for inode and check valid
static void inode_write(int index,char* inode_buff)
{
	char block_scratch[BLOCK_SIZE];
	block_read(my_sb->inode_start+(index/INODE_PER_BLOCK),block_scratch);
	bcopy(inode_buff,block_scratch+(index%INODE_PER_BLOCK)*sizeof(inode),sizeof(inode));
	block_write(my_sb->inode_start+(index/INODE_PER_BLOCK),block_scratch);
}
static void inode_init(inode *p,int type) // 0 for dir , 1 for file
{
	p->size=0;
	p->type=type;
	p->link_count=1;
	bzero(p->blocks,sizeof(p->blocks));
}
static int inode_create(int type)// 0 for dir 1 for file
{
	inode temp_inode;
	int alloc_index;
	alloc_index=inode_alloc();
	if(alloc_index<0)
		return -1;
	inode_init(&temp_inode,type);
	inode_write(alloc_index,&temp_inode)
	return alloc_index;
}
static void inode_free(int index)//free the inode, also free its data
{
	int temp=read_bitmap_block(INODE_BITMAP,index);
	inode inode_temp;
	if (temp)
	{
		
		inode_read(temp,&inode_temp)
		int used_data_blocks;//the block for indirect won't be count here 
		used_data_blocks=inode_temp.size/BLOCK_SIZE;
		if(inode_temp.size%BLOCK_SIZE)
			used_data_blocks++;
		if(used_data_blocks>DIRECT_BLOCK)//use indirect block
		{
			//use scratch here
			dblock_read(inode_temp.blocks[DIRECT_BLOCK],block_scratch);
			int i;
			for(i=0;i<=DIRECT_BLOCK;i++)
				dblock_free(inode_temp.blocks[i]);
			uint16_t *block_list=(uint16_t *)block_scratch;
			int end;
			end= used_data_blocks-DIRECT_BLOCK;
			for(i=0;i<end;i++)
				dblock_free(block_list[i]);
		}
		else{
			int i;
			for(i=0;i<used_data_blocks;i++)
				dblock_free(inode_temp.blocks[i]);
		}
		write_bitmap_block(INODE_BITMAP,index,0);
		my_sb->inode_count--;
		sb_write();
	}
}
//directories ---------------------------------------------------
static int dir_entry_add(int dir_index,int son_index,char *filename)
{
	//read_bitmap_block(INODE_BITMAP,dir_index)
	inode dir_inode;
	inode_read(dir_index,&dir_inode);
	int next_i;
	next_i=dir_inode.size/(sizeof(dir_entry));
	int next_i_inblock;
	next_i_inblock=next_i/DIR_ENTRY_PER_BLOCK;

	dir_entry new_entry;
	new_entry.inode_id=son_index;
	strcpy_safe(filename,new_entry.filename,MAX_FILE_NAME);


	if(next_i%DIR_ENTRY_PER_BLOCK==0)//need new block
	{
		if(next_i_inblock==)
	}
	else
	{
		if(next_i_inblock>=DIRECT_BLOCK){//indirect block
			dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
			uint16_t *block_list=(uint16_t *)block_scratch;
			next_i_inblock=block_list[next_i_inblock-DIRECT_BLOCK];
			dblock_read(next_i_inblock,block_scratch);
		}
		else
		{
			dblock_read(dir_inode.blocks[next_i_inblock],block_scratch);
			dir_entry *entry_list=(dir_entry *)block_scratch;
			entry_list[next_i%DIR_ENTRY_PER_BLOCK]=
			dblock_write(dir_inode.blocks[next_i_inblock],block_scratch);
		}
	}
	return -1;
}


//fs init ------------------------------------------------------
void fs_init( void) {
	block_init();
	/* More code HERE */
	//load super block
	my_sb = super_block_scratch;
	block_read(SUPER_BLOCK,super_block_scratch);
	if(my_sb->magic_num != MY_MAGIC) //main sb crash or not formatted
	{
		block_read(SUPER_BLOCK_BACKUP,super_block_scratch);
		if(my_sb->magic_num != MY_MAGIC)//need formatted
			mkfs();
			return;
		else
			block_write(SUPER_BLOCK,super_block_scratch);
	}
	//mount to root
	pwd=ROOT_DIR_ID;
	//clear fd_table
	bzero(fd_table,sizeof(fd_table));

	//load bitmaps
	block_read(sb->inode_bitmap_place,inode_bitmap_block_scratch);
	block_read(sb->dblock_bitmap_place,dblock_bitmap_block_scratch);
}

int fs_mkfs( void) {
	my_sb = super_block_scratch;
	my_sb->file_sys_size = FS_SIZE;
	my_sb->inode_bitmap_place = SUPER_BLOCK+1;
	my_sb->dblock_bitmap_place = SUPER_BLOCK+2;
	my_sb->inode_start = SUPER_BLOCK+3;
	my_sb->inode_count = 1;
	my_sb->dblock_start= SUPER_BLOCK+3+INODE_BLOCK_NUMBER;
	my_sb->magic_num=MY_MAGIC;
	sb_write();

//zero bitmaps
	bzero_block(SUPER_BLOCK+1);
	bzero_block(SUPER_BLOCK+2);
	bzero(inode_bitmap_block_scratch,BLOCK_SIZE);
	bzero(dblock_bitmap_block_scratch,BLOCK_SIZE);

	inode temp_root;
	inode_init(&temp_root,DIRECTORY);
	inode_write(ROOT_DIR_ID,&temp_root);


	//mount to root
	pwd = ROOT_DIR_ID;
	//clear fd_table
	bzero(fd_table,sizeof(fd_table));

	

	return 0;
}

int fs_open( char *fileName, int flags) {
	return -1;
}

int fs_close( int fd) {
	return -1;
}

int fs_read( int fd, char *buf, int count) {
	return -1;
}
	
int fs_write( int fd, char *buf, int count) {
	return -1;
}

int fs_lseek( int fd, int offset) {
	return -1;
}

int fs_mkdir( char *fileName) {
	return -1;
}

int fs_rmdir( char *fileName) {
	return -1;
}

int fs_cd( char *dirName) {
	return -1;
}

int fs_link( char *old_fileName, char *new_fileName) {
	return -1;
}

int fs_unlink( char *fileName) {
	return -1;
}

int fs_stat( char *fileName, fileStat *buf) {
	return -1;
}

