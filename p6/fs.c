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

//useful var--------------------------------------
static char block_scratch[BLOCK_SIZE];//inode read/write/free use scratch
static char block_scratch_1[BLOCK_SIZE];

static char inode_bitmap_block_scratch[BLOCK_SIZE];
static char dblock_bitmap_block_scratch[BLOCK_SIZE];

static char super_block_scratch[BLOCK_SIZE];

static super_b * my_sb;

static file_desc fd_table[MAX_OPEN_FILE_NUM];

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
	// char block_scratch[BLOCK_SIZE];
	block_read(my_sb->inode_start+(index/INODE_PER_BLOCK),block_scratch);
	bcopy(block_scratch+(index%INODE_PER_BLOCK)*sizeof(inode),inode_buff,sizeof(inode));
}
//caller prepare space for inode and check valid
static void inode_write(int index,char* inode_buff)
{
	// char block_scratch[BLOCK_SIZE];
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
	if(alloc_index<0){
		ERROR_MSG(("no enough inode space for new inode!\n"))
		return -1;
	}
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

//this func doesn't check same filename,so we may need to use find before we really insert one file to dir 
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
		int indirect_alloc_res,alloc_res;
		if(next_i_inblock>=DIRECT_BLOCK)//need indirect block
		{
 			if(next_i_inblock==DIRECT_BLOCK)//need indirect block ,but the indirect index block has not been alloced
			{
				indirect_alloc_res=dblock_alloc();
				if(indirect_alloc_res<0){
					ERROR_MSG(("no enough space for add new dir entry!\n"))
					return -1;
				}
				dir_inode.blocks[DIRECT_BLOCK]=indirect_alloc_res;//mount new indirect index block
			}
			//if(next_i_inblock>DIRECT_BLOCK)//need indirect block ,and the indirect index block has been alloced
 			//indirect index block
			dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
			uint16_t *block_list=(uint16_t *)block_scratch;
			alloc_res=dblock_alloc();
			if(alloc_res<0){
				if(next_i_inblock==DIRECT_BLOCK)
					dblock_free(indirect_alloc_res);
				ERROR_MSG(("no enough space for add new dir entry!\n"))
				return -1;
			}
			block_list[next_i_inblock-DIRECT_BLOCK]=alloc_res;//mount new block
			dblock_write(dir_inode.blocks[DIRECT_BLOCK],block_scratch);

		}
		else //need direct block
		{
			alloc_res=dblock_alloc();
			if(alloc_res<0){
				ERROR_MSG(("no enough space for add new dir entry!\n"))
				return -1;
			}
			dir_inode.blocks[next_i_inblock]=alloc_res;//mount new block
		}
			dblock_read(alloc_res,block_scratch);

			dir_entry *entry_list=(dir_entry *)block_scratch;
			entry_list[0]=new_entry;

			dblock_write(alloc_res,block_scratch);

	}
	else
	{
		if(next_i_inblock>=DIRECT_BLOCK){//indirect block
			dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
			uint16_t *block_list=(uint16_t *)block_scratch;
			next_i_inblock=block_list[next_i_inblock-DIRECT_BLOCK];//get real block no
		}
		else
		{
			//real block no is dir_inode.blocks[next_i_inblock]
			next_i_inblock=dir_inode.blocks[next_i_inblock];
		}

		dblock_read(next_i_inblock,block_scratch);

		dir_entry *entry_list=(dir_entry *)block_scratch;
		entry_list[next_i%DIR_ENTRY_PER_BLOCK]=new_entry;
		
		dblock_write(next_i_inblock,block_scratch);
	}

	dir_inode.size+=sizeof(dir_entry);
	inode_write(dir_index,&dir_inode);//update dir inode

	return 0;
}
//if match return inode id else return -1
//so we may need to use find before we really insert one file to dir 
static int dir_entry_find(int dir_index,char *filename)
{
	inode dir_inode;
	inode_read(dir_index,&dir_inode);
	int total_entry_num;
	total_entry_num=dir_inode.size/(sizeof(dir_entry));
	int total_block_num;
	total_block_num=(total_entry_num-1)/DIR_ENTRY_PER_BLOCK + 1;
	if(total_block_num>=DIRECT_BLOCK)
	{
		int i,j,entry_block,remain_blocks;
		for(i=0;i<DIRECT_BLOCK;i++)
		{
			entry_block=dir_inode.blocks[i];
			dblock_read(entry_block,block_scratch);
			dir_entry *entry_list=(dir_entry *)block_scratch;
			for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
				if(same_string(entry_list[j].file_name,filename))
					return entry_list[j].inode_id;
		}

		dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
		uint16_t *block_list=(uint16_t *)block_scratch;

		for(i=0;i<total_block_num-DIRECT_BLOCK-1;i++)
		{
			dblock_read(block_list[i],block_scratch_1);
			dir_entry *entry_list=(dir_entry *)block_scratch_1;
			for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
				if(same_string(entry_list[j].file_name,filename))
					return entry_list[j].inode_id;
		}
		//last block
		dblock_read(block_list[total_block_num-DIRECT_BLOCK-1],block_scratch_1)
		int final_end=total_entry_num%DIR_ENTRY_PER_BLOCK;
		dir_entry *entry_list=(dir_entry *)block_scratch_1;
		for(j=0;j<final_end;j++)
		{
			if(same_string(entry_list[j].file_name,filename))
				return entry_list[j].inode_id;
		}
	}
	else
	{
		int i,j,entry_block;
		for(i=0;i<total_block_num-1;i++)
		{
			entry_block=dir_inode.blocks[i];
			dblock_read(entry_block,block_scratch);
			dir_entry *entry_list=(dir_entry *)block_scratch;
			for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
				if(same_string(entry_list[j].file_name,filename))
					return entry_list[j].inode_id;
		}
		//last block
		int final_end=total_entry_num%DIR_ENTRY_PER_BLOCK;
		entry_block=dir_inode.blocks[total_block_num-1];
		dblock_read(entry_block,block_scratch);
		dir_entry *entry_list=(dir_entry *)block_scratch;
		for(j=0;j<final_end;j++)
		{
			if(same_string(entry_list[j].file_name,filename))
				return entry_list[j].inode_id;
		}
	}
	ERROR_MSG(("file doesn't exist!\n"))
	return -1;
}

static void swap_in_last_entry(int block_id,int in_block_id,int last_block_id,int in_last_block_id)
{
	if(block_id==last_block_id)//same block
	{
		if(in_block_id==in_last_block_id)//same
			return ;
		dblock_read(block_id,block_scratch);
		dir_entry *entry_list=(dir_entry *)block_scratch;
		entry_list[in_block_id]=entry_list[in_last_block_id];
		dblock_write(block_id,block_scratch);
	}
	else
	{
		dblock_read(block_id,block_scratch);
		dblock_read(last_block_id,block_scratch_1);
		dir_entry *entry_list=(dir_entry *)block_scratch;
		dir_entry *entry_list_last=(dir_entry *)block_scratch_1;
		entry_list[in_block_id]=entry_list_last[in_last_block_id];
		dblock_write(block_id,block_scratch);
	}
}

static int dir_entry_delete(int dir_index,char *filename)//unlink not integrated in this func , and this only affect one node
{
	inode dir_inode;
	inode_read(dir_index,&dir_inode);
	int total_entry_num;
	total_entry_num=dir_inode.size/(sizeof(dir_entry));
	int total_block_num;
	total_block_num=(total_entry_num-1)/DIR_ENTRY_PER_BLOCK + 1;
	
	int last_block_id;
	int in_last_block_id;
	if(total_block_num<=DIRECT_BLOCK){
		last_block_id=dir_inode.blocks[total_block_num-1];
	}
	else{
		dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
		uint16_t *block_list=(uint16_t *)block_scratch;
		last_block_id=block_list[total_block_num-1-DIRECT_BLOCK];
	}
	in_last_block_id=(total_entry_num-1)%DIR_ENTRY_PER_BLOCK;

	if(total_block_num>DIRECT_BLOCK)
	{
		int i,j,entry_block,remain_blocks;
		for(i=0;i<DIRECT_BLOCK;i++)//first we find in direct block
		{
			entry_block=dir_inode.blocks[i];
			dblock_read(entry_block,block_scratch);
			dir_entry *entry_list=(dir_entry *)block_scratch;
			for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
				if(same_string(entry_list[j].file_name,filename))
				{
					if(in_last_block_id==0)//need to free dblock
					{
						dblock_free(last_block_id);
						if(total_block_num==DIRECT_BLOCK+1)//also need to free indirect dblock
							dblock_free(dir_inode.blocks[DIRECT_BLOCK]);	
					}
					swap_in_last_entry(entry_block,j,,in_last_block_id);
					dir_inode.size-=sizeof(dir_entry);
					inode_write(dir_index,&dir_inode);
					return 0;
				}
		}

		dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
		uint16_t *block_list=(uint16_t *)block_scratch;

		for(i=0;i<total_block_num-DIRECT_BLOCK-1;i++)//the we find in indirect block
		{
			dblock_read(block_list[i],block_scratch_1);
			dir_entry *entry_list=(dir_entry *)block_scratch_1;
			for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
				if(same_string(entry_list[j].file_name,filename))
				{
					if(in_last_block_id==0)//need to free dblock
					{
						dblock_free(last_block_id);
						if(total_block_num==DIRECT_BLOCK+1)//also need to free indirect dblock
							dblock_free(dir_inode.blocks[DIRECT_BLOCK]);	
					}
					dir_inode.size-=sizeof(dir_entry);
					inode_write(dir_index,&dir_inode);
					return 0;
				}
		}
		//last block
		dblock_read(block_list[total_block_num-DIRECT_BLOCK-1],block_scratch_1)
		int final_end=total_entry_num%DIR_ENTRY_PER_BLOCK;
		dir_entry *entry_list=(dir_entry *)block_scratch_1;
		for(j=0;j<final_end;j++)
			if(same_string(entry_list[j].file_name,filename))
			{
				if(in_last_block_id==0)//need to free dblock
				{
					dblock_free(last_block_id);
					if(total_block_num==DIRECT_BLOCK+1)//also need to free indirect dblock
						dblock_free(dir_inode.blocks[DIRECT_BLOCK]);	
				}
				dir_inode.size-=sizeof(dir_entry);
				inode_write(dir_index,&dir_inode);
				return 0;
			}
	}
	else
	{
		int i,j,entry_block;
		for(i=0;i<total_block_num-1;i++)
		{
			entry_block=dir_inode.blocks[i];
			dblock_read(entry_block,block_scratch);
			dir_entry *entry_list=(dir_entry *)block_scratch;
			for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
				if(same_string(entry_list[j].file_name,filename))
				{
					if(in_last_block_id==0)//need to free dblock
						dblock_free(last_block_id);
					dir_inode.size-=sizeof(dir_entry);
					inode_write(dir_index,&dir_inode);
					return 0;
				}
		}
		//last block
		int final_end=total_entry_num%DIR_ENTRY_PER_BLOCK;
		entry_block=dir_inode.blocks[total_block_num-1];
		dblock_read(entry_block,block_scratch);
		dir_entry *entry_list=(dir_entry *)block_scratch;
		for(j=0;j<final_end;j++)
			if(same_string(entry_list[j].file_name,filename))
			{
				if(in_last_block_id==0)//need to free dblock
					dblock_free(last_block_id);
				dir_inode.size-=sizeof(dir_entry);
				inode_write(dir_index,&dir_inode);
				return 0;
			}
	}
	ERROR_MSG(("file doesn't exist!\n"))
	return -1;
}
//--- file descriptor helper---------------------------------------------
//these func just handle fd_table , won't delete inode & data
static int fd_open(int inode_id, int mode)
{
    int i;
    for (i = 0; i < MAX_OPEN_FILE_NUM; i++)
        if (fd_table[i].is_using==FALSE){
            fd_table[i].is_using = TRUE;
            fd_table[i].cursor = 0;
            fd_table[i].inode_id = inode_id;
            fd_table[i].mode = mode;
            return i;
        }
    return -1;
}
static void fd_close(int fd)
{
    fd_table[fd].is_using = FALSE;
}
static int fd_find_same_num(int inode_id,int mode)
{
	int i;
	int count=0;
	for(i=0;i<MAX_OPEN_FILE_NUM;i++)
		if(fd_table[i].is_using && fd_table[i].inode_id==inode_id)
			count++;
	return count;
}
//--- path resolve------------------------------------

//caller should save a copy of file_path , because this func will be changed the content 
static int path_dir_resolve(char * file_path,int temp_pwd)//this just resolve relative path and find inode
{
	int path_len=strlen(file_path);
	if(path_len<=0)
	{
		ERROR_MSG(("no path here!\n"))
		return -1;
	}
	int i;
	for(i=0;i<path_len;i++)
		if(file_path[i]=='/')
			break;
	file_path[i]='\0';
	int res=dir_entry_find(temp_pwd,file_path);
	if(res<0){
		ERROR_MSG(("%s doesn't exist!\n",))
		return -1;
	}
	if(i==path_len)//we reach the last item
		return res;
	inode temp;
	inode_read(res,&temp);
	if(temp.type!=DIRECTORY)
	{
		ERROR_MSG(("%s is a data file not a path!\n",file_path))
		return -1;
	}
	return path_dir_resolve(file_path+i+1,res,mode);
}

static int path_resolve(char * file_path , int temp_pwd ,int mode)//mode DIRECTORY 0, REAL_FILE 1 , find last dir 2
{
	int path_len=strlen(file_path);
	if(path_len>MAX_PATH_NAME){
		ERROR_MSG(("too long path!\n"))
		return -1;
	}
	//copy the path
	char path_buffer[MAX_PATH_NAME];
	bcopy(file_path,path_buffer,path_len);
	path_buffer[path_len]='\0';
	file_path=path_buffer;
	//trick to decide directory or real file
	if(mode==REAL_FILE && file_path[path_len-1]=='/')
	{
		ERROR_MSG(("try to find a path!\n"))
		return -1;
	}
	if(path_len<=0)
	{
		ERROR_MSG(("no path input!\n"))
		return -1;
	}
	if(file_path[0]=='/')//absolute path
	{
		if(path_len==1)//only root
		{
			if(mode==REAL_FILE)
			{
				ERROR_MSG(("/ is a directory not a data file\n"))
				return -1;
			}
			return ROOT_DIR_ID;
		}
		//trick to change str and temp_pwd
		temp_pwd=ROOT_DIR_ID;
		file_path++;
		path_len--;
	}
	int i;
	for(i=0;i<path_len;i++)
		if(file_path[i]=='/')
			break;
	if(i==path_len)
	{
		int res=dir_entry_find(temp_pwd,file_path);
		if(res<0){

			ERROR_MSG(("%s doesn't exist!\n",))
		}
		return res;
	}
	return path_resolve(file_path+i+1,temp_pwd,mode);
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
	write_bitmap_block(INODE_BITMAP,ROOT_DIR_ID,1);

	int res;
	res=dir_entry_add(ROOT_DIR_ID,ROOT_DIR_ID,".");
	if(res<0){
		bzero(super_block_scratch,BLOCK_SIZE);
		sb_write();
		return -1;
	}
	res=dir_entry_add(ROOT_DIR_ID,ROOT_DIR_ID,"..");
	if(res<0){
		bzero(super_block_scratch,BLOCK_SIZE);
		sb_write();
		return -1;
	}

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

