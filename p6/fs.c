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

//
static void new_block_write( int block, char *mem)
{
	int i;
	for(i=0;i<NEW_BLOCK_SIZE/BLOCK_SIZE;i++)
	{
		block_write(block*8+i,mem+i*BLOCK_SIZE);
	}
}
static void new_block_read( int block, char *mem)
{
	int i;
	for(i=0;i<NEW_BLOCK_SIZE/BLOCK_SIZE;i++)
	{
		block_read(block*8+i,mem+i*BLOCK_SIZE);
	}
}

//static helper func
static void strcpy_safe(char *src,char *dest,int dest_max_len)//dest max len without final '\0' buffer
{
	if(strlen(src)>dest_max_len)
		bcopy((unsigned char *)src,(unsigned char *)dest,dest_max_len);
	else
		bcopy((unsigned char *)src,(unsigned char *)dest,strlen(src));
	dest[dest_max_len]='\0';
}

//useful var--------------------------------------
static char block_scratch[NEW_BLOCK_SIZE];//inode read/write/free use scratch
static char block_scratch_1[NEW_BLOCK_SIZE];

static char inode_bitmap_block_scratch[NEW_BLOCK_SIZE];
static char dblock_bitmap_block_scratch[NEW_BLOCK_SIZE];

static char super_block_scratch[NEW_BLOCK_SIZE];

static super_b * my_sb= (super_b *)super_block_scratch;

static file_desc fd_table[MAX_OPEN_FILE_NUM];

static uint16_t pwd;//start from 0 as inode index

static uint16_t inode_bitmap_last=0;//MAX_FILE_COUNT-1;//start from end
static uint16_t dblock_bitmap_last=0;//DATA_BLOCK_NUMBER-1;//start from end

//superblock write helper------------------
static void sb_write()
{
	new_block_write(SUPER_BLOCK,super_block_scratch);
	new_block_write(SUPER_BLOCK_BACKUP,super_block_scratch);
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
	return (mask & the_byte)? 1:0;
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
		new_block_write(my_sb->dblock_bitmap_place,bitmap_block_scratch);
	else
		new_block_write(my_sb->inode_bitmap_place,bitmap_block_scratch);
}
static int find_next_free(int i_d)//must alloc(write 1) after this function find the result
{
	int i;
	int res;
	if(i_d){
		i=(dblock_bitmap_last+1)%DATA_BLOCK_NUMBER;
		while(i!=dblock_bitmap_last){
			res=read_bitmap_block(DBLOCK_BITMAP,i);
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
			res=read_bitmap_block(INODE_BITMAP,i);
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
		my_bzero_block(my_sb->dblock_start+search_res);
		return search_res;
	}
	ERROR_MSG(("alloc data block fail"))
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
	new_block_read(my_sb->dblock_start+index,block_buff);
}
//caller prepare space for whole data block
static void dblock_write(int index,char* block_buff)
{
	new_block_write(my_sb->dblock_start+index,block_buff);
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
static void inode_read(int index,inode* inode_buff)
{
	char temp_block_scratch[NEW_BLOCK_SIZE];
	new_block_read(my_sb->inode_start+(index/INODE_PER_BLOCK),temp_block_scratch);
	inode *inode_block_scratch=(inode *)temp_block_scratch;
	bcopy((unsigned char *)(inode_block_scratch+(index%INODE_PER_BLOCK)),(unsigned char *)inode_buff,sizeof(inode));
}
//caller prepare space for inode and check valid
static void inode_write(int index,inode* inode_buff)
{
	char temp_block_scratch[NEW_BLOCK_SIZE];
	new_block_read(my_sb->inode_start+(index/INODE_PER_BLOCK),temp_block_scratch);
	inode *inode_block_scratch=(inode *)temp_block_scratch;
	bcopy((unsigned char *)inode_buff,(unsigned char *)(inode_block_scratch+(index%INODE_PER_BLOCK)),sizeof(inode));
	new_block_write(my_sb->inode_start+(index/INODE_PER_BLOCK),temp_block_scratch);
}
static void inode_init(inode *p,int type) // 0 for dir , 1 for file
{
	p->size=0;
	p->type=type;
	p->link_count=1;
	bzero((char *)p->blocks,sizeof(uint16_t)*(DIRECT_BLOCK+1));
}
static int inode_create(int type)// 0 for dir 1 for file , create and init ! 
{
	inode temp_inode;
	int alloc_index;
	alloc_index=inode_alloc();
	if(alloc_index<0){
		ERROR_MSG(("no enough inode space for new inode!\n"))
		return -1;
	}
	inode_init(&temp_inode,type);
	inode_write(alloc_index,&temp_inode);
	return alloc_index;
}
static void inode_free(int index)//free the inode, also free its data
{
	int temp=read_bitmap_block(INODE_BITMAP,index);
	inode inode_temp;
	if (temp)
	{
		inode_read(index,&inode_temp);
		int used_data_blocks;//total blocks used , not included indirect index block
		used_data_blocks=(inode_temp.size-1+NEW_BLOCK_SIZE)/NEW_BLOCK_SIZE;
		if(used_data_blocks>DIRECT_BLOCK)//use indirect block
		{
			//use scratch here
			dblock_read(inode_temp.blocks[DIRECT_BLOCK],block_scratch);
			int i;
			for(i=0;i<=DIRECT_BLOCK;i++)
				dblock_free(inode_temp.blocks[i]);
			uint16_t *block_list=(uint16_t *)block_scratch;
			for(i=0;i<used_data_blocks-DIRECT_BLOCK;i++)
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
//this doesn't change inode.size
static int alloc_dblock_mount_to_inode(int inode_id)
{
	int alloc_res;
	inode temp;
	inode_read(inode_id,&temp);
	int next_block=(temp.size-1+NEW_BLOCK_SIZE)/NEW_BLOCK_SIZE;//start from 0 , mean the next in-inode blocks id
	if(next_block>MAX_BLOCKS_INDEX_IN_INODE)
	{
		ERROR_MSG(("beyond one inode can handle!\n"))
		return -1;
	}
	if(next_block>=DIRECT_BLOCK)//need indirect block
	{
		if(next_block==DIRECT_BLOCK)//need indirect block ,but the indirect index block has not been alloced
		{
			alloc_res=dblock_alloc();
			if(alloc_res<0)
				return -1;
			temp.blocks[DIRECT_BLOCK]=alloc_res;
		}
		dblock_read(temp.blocks[DIRECT_BLOCK],block_scratch);
		uint16_t *block_list=(uint16_t *)block_scratch;
		
		alloc_res=dblock_alloc();
		if(alloc_res<0){
			if(next_block==DIRECT_BLOCK)
				dblock_free(temp.blocks[DIRECT_BLOCK]);
			return -1;
		}
		block_list[next_block-DIRECT_BLOCK]=alloc_res;
		dblock_write(temp.blocks[DIRECT_BLOCK],block_scratch);
	}
	else
	{
		alloc_res=dblock_alloc();
		if(alloc_res<0)
			return -1;
		temp.blocks[next_block]=alloc_res;
	}
	// ERROR_MSG(("inode %d need a block in-inode id %d, alloc_res %d\n",inode_id,next_block,alloc_res))
	inode_write(inode_id,&temp);
	return alloc_res;
}

//directories ---------------------------------------------------

//this func doesn't check same filename,so we may need to use find before we really insert one file to dir 
static int dir_entry_add(int dir_index,int son_index,char *filename)
{
	//ERROR_MSG(("create new_file %s ,in dir %d,new_inode %d,\n",filename,dir_index,son_index))
	//read_bitmap_block(INODE_BITMAP,dir_index)
	inode dir_inode;
	inode_read(dir_index,&dir_inode);
	int next_i;
	next_i=dir_inode.size/(sizeof(dir_entry));
	int next_i_inblock;
	next_i_inblock=next_i/DIR_ENTRY_PER_BLOCK;

	dir_entry new_entry;
	new_entry.inode_id=son_index;
	bzero(new_entry.file_name,MAX_FILE_NAME);
	strcpy_safe(filename,new_entry.file_name,MAX_FILE_NAME);

	if(next_i%DIR_ENTRY_PER_BLOCK==0)//need new block
	{	
		int alloc_res=alloc_dblock_mount_to_inode(dir_index);
		if(alloc_res<0)
		{
			ERROR_MSG(("can't alloc dblock when insert dir_entry into dir\n"))
			return -1;
		}
		//update inode 
		inode_read(dir_index,&dir_inode);

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
	int total_entry_num=dir_inode.size/(sizeof(dir_entry));
	int total_block_num=(total_entry_num-1+DIR_ENTRY_PER_BLOCK)/DIR_ENTRY_PER_BLOCK;
	if(total_entry_num==0)
		return -1;

	if(total_block_num>DIRECT_BLOCK)
	{
		int i,j;

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
		dblock_read(block_list[total_block_num-DIRECT_BLOCK-1],block_scratch_1);
		int final_end=(total_entry_num-1)%DIR_ENTRY_PER_BLOCK;
		dir_entry *entry_list=(dir_entry *)block_scratch_1;
		for(j=0;j<=final_end;j++)
		{
			if(same_string(entry_list[j].file_name,filename))
				return entry_list[j].inode_id;
		}

		//trick to find in direct blocks
		total_block_num=DIRECT_BLOCK;
		total_entry_num=DIRECT_BLOCK*DIR_ENTRY_PER_BLOCK;
	}
	//direct blocks
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
	int final_end=(total_entry_num-1)%DIR_ENTRY_PER_BLOCK;
	entry_block=dir_inode.blocks[total_block_num-1];
	dblock_read(entry_block,block_scratch);
	dir_entry *entry_list=(dir_entry *)block_scratch;
	for(j=0;j<=final_end;j++)
	{
		if(same_string(entry_list[j].file_name,filename))
			return entry_list[j].inode_id;
	}
	

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

static int dir_entry_delete(int dir_index,char *filename)//unlink call in this func , and this only affect one node
{
	inode dir_inode;
	inode_read(dir_index,&dir_inode);
	int total_entry_num=dir_inode.size/(sizeof(dir_entry));
	int total_block_num=(total_entry_num-1+DIR_ENTRY_PER_BLOCK)/DIR_ENTRY_PER_BLOCK;
	if(total_entry_num==0)
		return -1;
	
	int last_block_id;
	int in_last_block_id=(total_entry_num-1+DIR_ENTRY_PER_BLOCK)%DIR_ENTRY_PER_BLOCK;

	int free_indirect_index_block_flag=0;
	if(total_block_num<=DIRECT_BLOCK){
		last_block_id=dir_inode.blocks[total_block_num-1];
	}
	else{
		dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
		uint16_t *block_list=(uint16_t *)block_scratch;
		last_block_id=block_list[total_block_num-1-DIRECT_BLOCK];
		if(total_block_num==DIRECT_BLOCK+1)
			free_indirect_index_block_flag=1;
	}
	

	if(total_block_num>DIRECT_BLOCK)
	{
		dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
		uint16_t *block_list=(uint16_t *)block_scratch;
		int i,j,entry_block;
		for(i=0;i<total_block_num-DIRECT_BLOCK-1;i++)//then we find in indirect block(without the last block)
		{
			entry_block=block_list[i];
			dblock_read(block_list[i],block_scratch_1);
			dir_entry *entry_list=(dir_entry *)block_scratch_1;
			for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
				if(same_string(entry_list[j].file_name,filename))
				{
					swap_in_last_entry(entry_block,j,last_block_id,in_last_block_id);
					if(in_last_block_id==0)//need to free dblock
					{
						dblock_free(last_block_id);
						if(free_indirect_index_block_flag)//also need to free indirect dblock
							dblock_free(dir_inode.blocks[DIRECT_BLOCK]);	
					}
					dir_inode.size-=sizeof(dir_entry);
					inode_write(dir_index,&dir_inode);
					return 0;
				}
		}
		//last block
		entry_block=block_list[total_block_num-DIRECT_BLOCK-1];
		dblock_read(block_list[total_block_num-DIRECT_BLOCK-1],block_scratch_1);

		int final_end=(total_entry_num-1)%DIR_ENTRY_PER_BLOCK;
		dir_entry *entry_list=(dir_entry *)block_scratch_1;
		for(j=0;j<=final_end;j++)
			if(same_string(entry_list[j].file_name,filename))
			{
				swap_in_last_entry(entry_block,j,last_block_id,in_last_block_id);
				if(in_last_block_id==0)//need to free dblock
				{
					dblock_free(last_block_id);
					if(free_indirect_index_block_flag)//also need to free indirect dblock
						dblock_free(dir_inode.blocks[DIRECT_BLOCK]);	
				}
				
				dir_inode.size-=sizeof(dir_entry);
				inode_write(dir_index,&dir_inode);
				return 0;
			}

		//trick to find in direct blocks
		total_block_num=DIRECT_BLOCK;
		total_entry_num=DIRECT_BLOCK*DIR_ENTRY_PER_BLOCK;

	}

	int i,j,entry_block;
	for(i=0;i<total_block_num-1;i++)
	{
		entry_block=dir_inode.blocks[i];
		dblock_read(entry_block,block_scratch);
		dir_entry *entry_list=(dir_entry *)block_scratch;
		for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
			if(same_string(entry_list[j].file_name,filename))
			{
				swap_in_last_entry(entry_block,j,last_block_id,in_last_block_id);
				if(in_last_block_id==0)//need to free dblock
				{
					dblock_free(last_block_id);
					if(free_indirect_index_block_flag)//also need to free indirect dblock
						dblock_free(dir_inode.blocks[DIRECT_BLOCK]);	
				}
				dir_inode.size-=sizeof(dir_entry);
				inode_write(dir_index,&dir_inode);
				return 0;
			}
	}
	//last block
	int final_end=(total_entry_num-1)%DIR_ENTRY_PER_BLOCK;
	entry_block=dir_inode.blocks[total_block_num-1];
	dblock_read(entry_block,block_scratch);
	dir_entry *entry_list=(dir_entry *)block_scratch;
	for(j=0;j<=final_end;j++)
		if(same_string(entry_list[j].file_name,filename))
		{
			swap_in_last_entry(entry_block,j,last_block_id,in_last_block_id);
			if(in_last_block_id==0)//need to free dblock
			{
				dblock_free(last_block_id);
				if(free_indirect_index_block_flag)//also need to free indirect dblock
					dblock_free(dir_inode.blocks[DIRECT_BLOCK]);	
			}
			dir_inode.size-=sizeof(dir_entry);
			inode_write(dir_index,&dir_inode);
			return 0;
		}


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
    ERROR_MSG(("Not enough file descriptor!\n"))
    return -1;
}
static void fd_close(int fd)
{
    fd_table[fd].is_using = FALSE;
}
static int fd_find_same_num(int inode_id)
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
static int rel_path_dir_resolve(char * file_path,int temp_pwd)//this just resolve relative path and find inode
{
	int path_len=strlen(file_path);
	if(path_len<=0)
	{
		return temp_pwd;
	}
	int i;
	for(i=0;i<path_len;i++)
		if(file_path[i]=='/'){
			file_path[i]='\0';
			break;
		}
	int res=dir_entry_find(temp_pwd,file_path);
	if(res<0){
		// ERROR_MSG(("%s doesn't exist!\n",file_path))
		return -1;
	}
	if(i==path_len)//we reach the last item
		return res;
	inode temp;
	inode_read(res,&temp);
	if(temp.type!=MY_DIRECTORY)
	{
		ERROR_MSG(("%s is a data file not a path!\n",file_path))
		return -1;
	}
	return rel_path_dir_resolve(file_path+i+1,res);
}
//mode MY_DIRECTORY 0, REAL_FILE 1 , find last dir 2
//mode 0 allow input d/ or d , 1 find file's inode , 2 find it's parent's inode
static int path_resolve(char * file_path , int temp_pwd ,int mode)
{
	if(file_path==NULL){
		ERROR_MSG(("No path input!\n"))
		return -1;
	}
	int path_len=strlen(file_path);
	if(path_len>MAX_PATH_NAME){
		ERROR_MSG(("too long path!\n"))
		return -1;
	}
	if(path_len==0)
	{
		ERROR_MSG(("no path input!\n"))
		return -1;
	}

	//copy the path
	char path_buffer[MAX_PATH_NAME+1];
	bcopy((unsigned char *)file_path,(unsigned char *)path_buffer,path_len);
	path_buffer[path_len]='\0';
	file_path=path_buffer;
	
	if(file_path[0]=='/')//absolute path
	{
		//trick to change str and temp_pwd
		temp_pwd=ROOT_DIR_ID;
		file_path++;
		path_len--;
	}

	if(mode==2)//to find the last dir
	{
		int i;
		for(i=path_len-1;i>=0;i--)//cut the last term
			if(file_path[i]=='/'){
				file_path[i+1]='\0';
				break;
			}
		//find dir
		if(i==-1)
			return temp_pwd;
		else if(i==0)
			return ROOT_DIR_ID;
		else
			return path_resolve(file_path,temp_pwd,MY_DIRECTORY);
	}

	
	//real file or last dir mode need to check last char
	if(mode!=MY_DIRECTORY)
	{
		if(path_len==0 || file_path[path_len-1]=='/')//root or end up with /
		{
			ERROR_MSG(("try to find a file but input a path!\n"))
			return -1;
		}
	}
	else if(file_path[path_len-1]=='/')//dir allow input: cd abc/ or cd abc  
		file_path[path_len-1]='\0';
	
	//now we have a prepared relative path
	return rel_path_dir_resolve(file_path,temp_pwd);
}

//fs init ------------------------------------------------------
void fs_init( void) {
	block_init();
	/* More code HERE */
	//load super block
	my_sb = (super_b *)super_block_scratch;
	new_block_read(SUPER_BLOCK,super_block_scratch);
	if(my_sb->magic_num != MY_MAGIC) //main sb crash or not formatted
	{
		new_block_read(SUPER_BLOCK_BACKUP,super_block_scratch);
		if(my_sb->magic_num != MY_MAGIC)//need formatted
		{
			fs_mkfs();
			return;
		}
		else
			new_block_write(SUPER_BLOCK,super_block_scratch);
	}
	//mount to root
	pwd=(uint16_t)ROOT_DIR_ID;
	//clear fd_table
	bzero((char *)fd_table,sizeof(fd_table));

	//load bitmaps
	new_block_read(my_sb->inode_bitmap_place,inode_bitmap_block_scratch);
	new_block_read(my_sb->dblock_bitmap_place,dblock_bitmap_block_scratch);
}

int fs_mkfs( void) {
	my_sb = (super_b *)super_block_scratch;
	my_sb->file_sys_size = FS_SIZE;
	my_sb->inode_bitmap_place = SUPER_BLOCK+1;
	my_sb->dblock_bitmap_place = SUPER_BLOCK+2;
	my_sb->inode_start = SUPER_BLOCK+3;
	my_sb->inode_count = 1;
	my_sb->dblock_start= SUPER_BLOCK+3+INODE_BLOCK_NUMBER;
	my_sb->magic_num=MY_MAGIC;
	sb_write();
	//zero bitmaps
	my_bzero_block(my_sb->inode_bitmap_place);
	my_bzero_block(my_sb->dblock_bitmap_place);
	bzero(inode_bitmap_block_scratch,NEW_BLOCK_SIZE);
	bzero(dblock_bitmap_block_scratch,NEW_BLOCK_SIZE);
	//reset pointers
	inode_bitmap_last=0;
	dblock_bitmap_last=0;
	
	inode temp_root;
	inode_init(&temp_root,MY_DIRECTORY);
	inode_write(ROOT_DIR_ID,&temp_root);
	write_bitmap_block(INODE_BITMAP,ROOT_DIR_ID,1);

	int res;
	res=dir_entry_add(ROOT_DIR_ID,ROOT_DIR_ID,".");
	if(res<0){
		bzero(super_block_scratch,NEW_BLOCK_SIZE);
		sb_write();
		return -1;
	}
	res=dir_entry_add(ROOT_DIR_ID,ROOT_DIR_ID,"..");
	if(res<0){
		bzero(super_block_scratch,NEW_BLOCK_SIZE);
		sb_write();
		return -1;
	}

	//mount to root
	pwd = ROOT_DIR_ID;
	//clear fd_table
	bzero((char *)fd_table,sizeof(fd_table));

	return 0;
}

int fs_open( char *fileName, int flags) {
	int path_res=path_resolve(fileName,pwd,MY_DIRECTORY);
	if(flags!=FS_O_RDONLY && flags!= FS_O_WRONLY && flags!= FS_O_RDWR)
		return -1;
	int new_fd=fd_open(path_res,flags);
	if(new_fd<0)
		return -1;
	if(path_res<0)
	{
		if(flags==FS_O_RDONLY)//read only can not create file
		{
			fd_close(new_fd);
			ERROR_MSG(("%s doesn't exist,and try to open as read-only\n",fileName))
			return -1;
		}
		else
		{
			path_res=path_resolve(fileName,pwd,2);//try to find parent dir and create?
			if(path_res<0)
			{
				fd_close(new_fd);
				ERROR_MSG(("%s doesn't exist,and its parent dir doesn't exist either\n",fileName));
				return -1;
			}
			int i;
			for(i=strlen(fileName)-1;i>=0;i--)//cut the last term
				if(fileName[i]=='/')
					break;
			i++;
			int new_inode=inode_create(REAL_FILE);
			if(new_inode<0)
			{
				fd_close(new_fd);
				ERROR_MSG(("can't create inode when try to open a new file\n"));
				return -1;
			}
			dir_entry_add(path_res,new_inode,fileName+i);
			fd_table[new_fd].inode_id=new_inode;
		}
	}
	else{
		inode temp;
		inode_read(path_res,&temp);
		if(flags!=FS_O_RDONLY && temp.type == MY_DIRECTORY)
		{
			ERROR_MSG(("%s is a directory,but try to open as writable\n",fileName))
			fd_close(new_fd);
			return -1;
		}
		fd_table[new_fd].inode_id=path_res;
	}
	return new_fd;
}

int fs_close( int fd) {
	if(fd<0||fd>=MAX_OPEN_FILE_NUM)
	{
		ERROR_MSG(("Wrong fd input!\n"))
		return -1;
	}
	if(fd_table[fd].is_using==FALSE)
	{
		ERROR_MSG(("fd %d is not using!",fd))
		return -1;
	}
	fd_close(fd);

	//check whether we need to delete the file
	if(fd_find_same_num(fd_table[fd].inode_id)==0)
	{
		inode temp;
		inode_read(fd_table[fd].inode_id,&temp);
		if(temp.link_count==0)//need to free the file
			inode_free(fd_table[fd].inode_id);
	}
	return fd;
}

int fs_read( int fd, char *buf, int count) {
	if(count<0)
	{
		ERROR_MSG(("Wrong count input!\n"))
		return -1;
	}
	if(fd<0||fd>=MAX_OPEN_FILE_NUM)
	{
		ERROR_MSG(("Wrong fd input!\n"))
		return -1;
	}
	if(fd_table[fd].is_using==FALSE)
	{
		ERROR_MSG(("fd %d is not using!",fd))
		return -1;
	}
	if(fd_table[fd].mode==FS_O_WRONLY)
	{
		ERROR_MSG(("can't read the file open as write-only file"))
		return -1;
	}
	if (count==0)
	{
		return 0;
	}
	inode temp_file;
	inode_read(fd_table[fd].inode_id,&temp_file);
	
	int real_count=0;
	if(fd_table[fd].cursor>=temp_file.size)
		return 0;
	if(fd_table[fd].cursor+count>temp_file.size)
		count=temp_file.size-fd_table[fd].cursor;

	int end_block=(fd_table[fd].cursor+count-1)/NEW_BLOCK_SIZE;
	int in_end_block_cursor=(fd_table[fd].cursor+count-1)%NEW_BLOCK_SIZE;
	
	while(real_count<count)
	{
		int now_block=fd_table[fd].cursor/NEW_BLOCK_SIZE;
		int now_block_id;
		if(now_block>=DIRECT_BLOCK)
		{
			dblock_read(temp_file.blocks[DIRECT_BLOCK],block_scratch);
			uint16_t *block_list=(uint16_t *)block_scratch;
			now_block_id=block_list[now_block-DIRECT_BLOCK];
		}
		else
		{
			now_block_id=temp_file.blocks[now_block];
		}
		dblock_read(now_block_id,block_scratch);

		int rdy_count;
		if(now_block<end_block)
			rdy_count=NEW_BLOCK_SIZE-fd_table[fd].cursor%NEW_BLOCK_SIZE;
		else
			rdy_count=in_end_block_cursor-fd_table[fd].cursor%NEW_BLOCK_SIZE+1;
		bcopy((unsigned char *)(block_scratch+fd_table[fd].cursor%NEW_BLOCK_SIZE),(unsigned char *)buf,rdy_count);
		buf+=rdy_count;
		real_count+=rdy_count;
		fd_table[fd].cursor+=rdy_count;
	}
	return real_count;
}
	
int fs_write( int fd, char *buf, int count) {
	if(count<0)
	{
		ERROR_MSG(("Wrong count input!\n"))
		return -1;
	}
	if(fd<0||fd>=MAX_OPEN_FILE_NUM)
	{
		ERROR_MSG(("Wrong fd input!\n"))
		return -1;
	}
	if(fd_table[fd].is_using==FALSE)
	{
		ERROR_MSG(("fd %d is not using!",fd))
		return -1;
	}
	if(fd_table[fd].mode==FS_O_RDONLY)
	{
		ERROR_MSG(("can't write the file open as read-only file"))
		return -1;
	}
	if (count==0)
	{
		return 0;
	}
	inode temp_file;
	inode_read(fd_table[fd].inode_id,&temp_file);
	int temp_size=temp_file.size;

	int total_block_num=(temp_file.size-1+NEW_BLOCK_SIZE)/NEW_BLOCK_SIZE;
	int end_block_num=(fd_table[fd].cursor+count-1+NEW_BLOCK_SIZE)/NEW_BLOCK_SIZE;
	int in_end_block_cursor=(fd_table[fd].cursor+count-1)%NEW_BLOCK_SIZE;
	while(total_block_num<end_block_num)
	{
		if(alloc_dblock_mount_to_inode(fd_table[fd].inode_id)<0)
		{
			end_block_num=total_block_num;
			in_end_block_cursor=NEW_BLOCK_SIZE-1;
			break;
		}
		inode_read(fd_table[fd].inode_id,&temp_file);
		temp_file.size+=NEW_BLOCK_SIZE;
		inode_write(fd_table[fd].inode_id,&temp_file);
		total_block_num++;
	}
	//there may be some new dblocks , so we need the newest inode
	//inode_read(fd_table[fd].inode_id,&temp_file);
	
	count=(end_block_num-1)*NEW_BLOCK_SIZE+in_end_block_cursor-fd_table[fd].cursor+1;

	if(fd_table[fd].cursor+count>temp_size)
		temp_file.size=fd_table[fd].cursor+count;
	else
		temp_file.size=temp_size;

	inode_write(fd_table[fd].inode_id,&temp_file);
	
	int real_count=0;
	while(real_count<count)
	{
		int now_block=fd_table[fd].cursor/NEW_BLOCK_SIZE;
		int now_block_id;
		if(now_block>=DIRECT_BLOCK)
		{
			dblock_read(temp_file.blocks[DIRECT_BLOCK],block_scratch);
			uint16_t *block_list=(uint16_t *)block_scratch;
			now_block_id=block_list[now_block-DIRECT_BLOCK];
		}
		else
		{
			now_block_id=temp_file.blocks[now_block];
		}
		dblock_read(now_block_id,block_scratch);

		int rdy_count;
		if(now_block<end_block_num-1)
			rdy_count=NEW_BLOCK_SIZE-fd_table[fd].cursor%NEW_BLOCK_SIZE;
		else
			rdy_count=in_end_block_cursor-fd_table[fd].cursor%NEW_BLOCK_SIZE+1;
		bcopy((unsigned char *)buf,(unsigned char *)(block_scratch+fd_table[fd].cursor%NEW_BLOCK_SIZE),rdy_count);
		dblock_write(now_block_id,block_scratch);
		buf+=rdy_count;
		real_count+=rdy_count;
		fd_table[fd].cursor+=rdy_count;
	}
	return real_count;
}

//we assume the start position of fs_lseek is always SEEK_SET = 0
int fs_lseek( int fd, int offset) {
	if(fd<0||fd>=MAX_OPEN_FILE_NUM)
	{
		ERROR_MSG(("Wrong fd !\n"))
		return -1;
	}
	if(fd_table[fd].is_using==FALSE)
	{
		ERROR_MSG(("The fd isn't open!\n"))
		return -1;
	}
	if(offset<0)
	{
		ERROR_MSG(("offset <0 !\n"))
		return -1;
	}
	int old_cursor=fd_table[fd].cursor;

	fd_table[fd].cursor=offset;
	return old_cursor;
}

int fs_mkdir_pwd( char *fileName) {
	if(strlen(fileName)>MAX_FILE_NAME)
	{
		ERROR_MSG(("Too long file name!\n"))
		return -1;
	}
	if(dir_entry_find(pwd,fileName)>=0)
	{
		ERROR_MSG(("Already have a same name file !\n"))
		return -1;
	}
	
	int new_inode=inode_create(MY_DIRECTORY);
	if(new_inode<0)
		return -1;
	if(dir_entry_add(new_inode,new_inode,".")<0){
		inode_free(new_inode);
		return -1;
	}
	if(dir_entry_add(new_inode,pwd,"..")<0){
		inode_free(new_inode);
		return -1;
	}
	if(dir_entry_add(pwd,new_inode,fileName)<0){
		inode_free(new_inode);
		return -1;
	}
	return new_inode;
}
int fs_mkdir(char *fileName)
{
	
	int path_len=strlen(fileName);
	
	int path_res=path_resolve(fileName,pwd,2);//try to find parent dir
	if(path_res<0)//create parent dir
	{
		char path_buffer[MAX_PATH_NAME];
		bzero(path_buffer,MAX_PATH_NAME);
		
		bcopy((unsigned char *)fileName,(unsigned char *)path_buffer,path_len);
		int i;
		for(i=path_len-1;i>=0;i--)
			if(path_buffer[i]=='/')
			{
				path_buffer[i]=0;
				break;
			}
		fs_mkdir(path_buffer);
	}
	path_res=path_resolve(fileName,pwd,2);//get parent dir
	
	if(path_res>=0)//we need to make sure parent dir is successfully created
	{
		int temp_pwd=pwd;
		
		int i;
		for(i=path_len-1;i>=0;i--)//cut the last term
			if(fileName[i]=='/')
				break;
		i++;
		
		pwd=path_res;
		fs_mkdir_pwd(fileName+i);
		pwd=temp_pwd;
	}
	return 0;
}
//we assume -r is set
int fs_rmdir_part( char *fileName) {
	int dir_res=path_resolve(fileName,pwd,MY_DIRECTORY);
	if(dir_res<0)
	{
		ERROR_MSG(("The directory doesn't exist!\n"))
		return -1;
	}
	inode dir_inode;
	inode_read(dir_res,&dir_inode);
	if(dir_inode.type!=MY_DIRECTORY)
	{
		ERROR_MSG(("%s is not a directory\n",fileName))
		return -1;
	}
	int total_entry_num=dir_inode.size/(sizeof(dir_entry));
	int total_block_num=(total_entry_num-1+DIR_ENTRY_PER_BLOCK)/DIR_ENTRY_PER_BLOCK;
	if(total_block_num==0)
		return 0;

	//save the pwd and change it
	int save_pwd=pwd;
	pwd=dir_res;
	//because we may need recursion , we need to use local variables block scratch and block scratch1

	char block_scratch[NEW_BLOCK_SIZE];
	char block_scratch_1[NEW_BLOCK_SIZE];

	if(total_block_num>DIRECT_BLOCK)
	{
		int i,j;
		dblock_read(dir_inode.blocks[DIRECT_BLOCK],block_scratch);
		uint16_t *block_list=(uint16_t *)block_scratch;

		for(i=0;i<total_block_num-DIRECT_BLOCK-1;i++)
		{
			dblock_read(block_list[i],block_scratch_1);
			dir_entry *entry_list=(dir_entry *)block_scratch_1;
			for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
			{
				inode temp_j;
				inode_read(entry_list[j].inode_id,&temp_j);
				if(temp_j.type==MY_DIRECTORY)
				{
					if(same_string(entry_list[j].file_name,".") || same_string(entry_list[j].file_name,".."))
						continue;
					fs_rmdir_part(entry_list[j].file_name);
				}
				else
					fs_unlink(entry_list[j].file_name);
			}
		}
		//last block
		dblock_read(block_list[total_block_num-DIRECT_BLOCK-1],block_scratch_1);
		int final_end=(total_entry_num-1+DIR_ENTRY_PER_BLOCK)%DIR_ENTRY_PER_BLOCK;
		dir_entry *entry_list=(dir_entry *)block_scratch_1;
		for(j=0;j<=final_end;j++)
		{
			inode temp_j;
			inode_read(entry_list[i].inode_id,&temp_j);
			if(temp_j.type==MY_DIRECTORY)
			{
				if(same_string(entry_list[j].file_name,".") || same_string(entry_list[j].file_name,".."))
					continue;
				fs_rmdir_part(entry_list[j].file_name);
			}
			else
				fs_unlink(entry_list[j].file_name);
		}
		//trick to find in direct blocks
		total_block_num=DIRECT_BLOCK;
		total_entry_num=DIRECT_BLOCK*DIR_ENTRY_PER_BLOCK;

	}
	//direct blocks
	int i,j,entry_block;
	for(i=0;i<total_block_num-1;i++)
	{
		entry_block=dir_inode.blocks[i];
		dblock_read(entry_block,block_scratch);
		dir_entry *entry_list=(dir_entry *)block_scratch;
		for(j=0;j<DIR_ENTRY_PER_BLOCK;j++)
		{
			inode temp_j;
			inode_read(entry_list[j].inode_id,&temp_j);
			if(temp_j.type==MY_DIRECTORY)
			{
				if(same_string(entry_list[j].file_name,".") || same_string(entry_list[j].file_name,".."))
					continue;
				fs_rmdir_part(entry_list[j].file_name);
			}
			else
				fs_unlink(entry_list[j].file_name);
		}
	}
	//last block
	int final_end=(total_entry_num-1+DIR_ENTRY_PER_BLOCK)%DIR_ENTRY_PER_BLOCK;
	entry_block=dir_inode.blocks[total_block_num-1];
	dblock_read(entry_block,block_scratch);
	dir_entry *entry_list=(dir_entry *)block_scratch;
	for(j=0;j<=final_end;j++)
	{
		inode temp_j;
		inode_read(entry_list[j].inode_id,&temp_j);
		if(temp_j.type==MY_DIRECTORY)
		{
			if(same_string(entry_list[j].file_name,".") || same_string(entry_list[j].file_name,".."))
				continue;
			fs_rmdir_part(entry_list[j].file_name);
		}
		else
			fs_unlink(entry_list[j].file_name);
	}
	pwd=save_pwd;
	if(dir_res!=ROOT_DIR_ID)
		inode_free(dir_res);
	else
		return -1;
	return 0;
}
int fs_rmdir(char *fileName)
{
	if(fs_rmdir_part(fileName)==0){
		int parent_res=path_resolve(fileName,pwd,2);//try to find parent dir 
		int i;
		for(i=strlen(fileName)-1;i>=0;i--)//cut the last term
			if(fileName[i]=='/')
				break;
		i++;
		dir_entry_delete(parent_res,fileName+i);
		return 0;
	}
	else
		return -1;
}

int fs_cd( char *dirName) {
	int path_res=path_resolve(dirName,pwd,MY_DIRECTORY);
	if(path_res<0)
		return -1;
	inode temp_inode;
	inode_read(path_res,&temp_inode);
	if(temp_inode.type!=MY_DIRECTORY)
	{
		ERROR_MSG(("%s is not a dir\n",dirName));
		return -1;
	}
	pwd=path_res;
	return 0;
}

int fs_link( char *old_fileName, char *new_fileName) {
	int old_res=path_resolve(old_fileName,pwd,REAL_FILE);
	if(old_res<0)
	{
		ERROR_MSG(("The old file doesn't exist!\n"))
		return -1;
	}
	inode temp;
	inode_read(old_res,&temp);
	if(temp.type==MY_DIRECTORY)
	{
		ERROR_MSG(("Try to link a directory!\n"))
		return -1;
	}
	int new_res=path_resolve(new_fileName,pwd,REAL_FILE);
	if(new_res>0)
	{
		ERROR_MSG(("new filename already exist!\n"))
		return -1;
	}
	new_res=path_resolve(new_fileName,pwd,2);//try to find parent dir 
	if(new_res<0)
	{
		ERROR_MSG(("%s 's parent dir doesn't exist\n",new_fileName));
		return -1;
	}
	int i;
	for(i=strlen(new_fileName)-1;i>=0;i--)//cut the last term
		if(new_fileName[i]=='/')
			break;
	i++;
	dir_entry_add(new_res,old_res,new_fileName+i);
	temp.link_count++;
	inode_write(old_res,&temp);
	return 0;
}

int fs_unlink( char *fileName) {
	int res=path_resolve(fileName,pwd,REAL_FILE);
	if(res<0)
	{
		ERROR_MSG(("The file doesn't exist!\n"))
		return -1;
	}
	inode temp;
	inode_read(res,&temp);
	if(temp.type==MY_DIRECTORY)
	{
		ERROR_MSG(("try to unlink a dir!\n"))
		return -1;
	}
	temp.link_count--;
	inode_write(res,&temp);
	if(temp.link_count==0)
	{
		if(fd_find_same_num(res)==0)
			inode_free(res);
	}

	int parent_res=path_resolve(fileName,pwd,2);//try to find parent dir 
	int i;
	for(i=strlen(fileName)-1;i>=0;i--)//cut the last term
		if(fileName[i]=='/')
			break;
	i++;
	dir_entry_delete(parent_res,fileName+i);
	return 0;
}

int fs_stat( char *fileName, fileStat *buf) {
	int res=path_resolve(fileName,pwd,REAL_FILE);
	if(res<0)
	{
		ERROR_MSG(("The file doesn't exist!\n"))
		return -1;
	}
	inode temp;
	inode_read(res,&temp);
	buf->inodeNo=res;
	buf->type=temp.type+1;
	buf->links=temp.link_count;
	buf->size=temp.size;
	buf->numBlocks=(temp.size-1+NEW_BLOCK_SIZE)/NEW_BLOCK_SIZE;
	if(buf->numBlocks>DIRECT_BLOCK)
		buf->numBlocks++;
	return 0;
}

int fs_cd_inode_id(int dir_id)
{
	inode temp;
	inode_read(dir_id,&temp);
	if(temp.type!=MY_DIRECTORY)
		return -1;
	pwd=dir_id;
	return 0;
}
