#include "util.h"
#include "common.h"
#include "shellutil.h"
#include "fs.h"
#include <stdlib.h>
#include <stdio.h>

int superblock_test(int s1,int s2)
{
    //S1 mkfs and write file
    fs_init();
    if(fs_mkfs() < 0){
        printf("mkfs error!");
        return -1;
    }
    int fd, i,count;
    char buf[] = "superblock test\n";
    count = strlen(buf);

    if ((fd = fs_open("test", FS_O_RDWR)) < 0){
        printf("create file error!\n");
        return -1;
    }
    if(fs_write(fd, buf, count) <= 0){
        printf("write data error!\n");
        return -1;
    }

    fs_close(fd);
    if ((fd = fs_open("test", FS_O_RDONLY)) < 0){
        printf("open file error!\n");
        return -1;
    }
    fs_lseek(fd,0);
    char out[count];
    if ((i = fs_read(fd, out, count)) !=count){
        printf("read data error!\n");
        return -1;
    }
    fs_close(fd);

    for(i=0;i<count;i++){
        if(out[i] != buf[i]){
            printf("data not correct !\n") ;
          return -1;
        }
    }

    //S2 corrupt one sb
    char clear[512];
    bzero(clear,512);
    for(i=0;i<8;i++)
      block_write(8*s1+i,clear);

    //S3 init and test
    fs_init();
    if ((fd = fs_open("test", FS_O_RDONLY))< 0){
        printf("open file error after corrupted!\n");
        return -1;
    }
    fs_lseek(fd,0);
    if ((i = fs_read(fd, out, count)) !=count){
        printf("read data error! after corrupted\n");
        return -1;
    }
    fs_close(fd);

    for(i=0;i<count;i++){
        if(out[i] != buf[i]){
            printf("data not correct after corrupted!\n") ;
          return -1;
        }
    }

    //S4 corrupt both sb
    for(i=0;i<8;i++){
        block_write(8*s1+i,clear);
        block_write(8*s2+i,clear);
    }
    //S5 init and should fail
    fs_init();
    if ((fd = fs_open("test", FS_O_RDONLY)) < 0){
        printf("superblock test pass!\n");
        return 0;
    }else{
        printf ("superblock location INCORRECT!\n");
        return -1;
    }

    return 0;
}
int path_lookup_test(){
    fs_init();
    if(fs_mkfs() < 0){
        printf("mkfs error!");
        return -1;
    }
    int fd, i,count;
    char buf[] = "path lookup test\n";
    count = strlen(buf);
    fs_mkdir("/x/y/z");
    if ((fd = fs_open("/x/y/z/a", FS_O_RDWR)) < 0){
        printf("create full path error!\n");
        return -1;
    }
    if(fs_write(fd, buf, count) <= 0){
        printf("write data error!\n");
        return -1;
    }
    fs_close(fd);
    if (fs_cd("/x/y/z") < 0){
        printf("cd full path error!\n");
        return -1;
    }
    if (fs_cd("..") < 0){
        printf("cd parent  error!\n");
        return -1;
    }
    
    if (fs_cd("/") < 0){
        printf("cd root error!\n");
        return -1;
    }
    fs_cd("x");
    fs_cd("y");
    fs_cd("z");
    if ((fd = fs_open("a", FS_O_RDONLY)) < 0){
        printf("open file error!\n");
        return -1;
    }
    fs_lseek(fd,0);
    char out[count];
    if ((i = fs_read(fd, out, count)) !=count){
        printf("read data error!\n");
        return -1;
    }
    fs_close(fd);
    
    for(i=0;i<count;i++){
        if(out[i] != buf[i]){
            printf("data not correct !\n") ;
            return -1;
        }
    }
    
    printf("path lookup test pass!\n");
    return 0;
}
int rmdir_test(int fs_size,int inode, int file_count, int other_use)
{
    if(fs_size <= 10 || inode <= 0)
        return -1;

    fs_init();
    if(fs_mkfs() < 0){
        printf("mkfs error!");
        return -1;
    }
    int fd, i,count;
    char buf[] = "path lookup test\n";
    count = strlen(buf);
    fs_mkdir("/x/y/z");
    if ((fd = fs_open("/x/y/z/a", FS_O_RDWR)) < 0){
        printf("create full path error!\n");
        return -1;
    }
    fs_close(fd);
    
    if ( fs_cd("/x/y/z") < 0){
        printf("cd full path error!\n");
        return -1;
    }
    
    for(i=0;i<50;i++){
        char name[2];
        itoa(i,name);
        if ((fd = fs_open(name, FS_O_RDWR)) < 0){
            printf("create file error!\n");
            return -1;
        }
        if(fs_write(fd, buf, count) <= 0){
            printf("write data error!\n");
            return -1;
        }
        fs_close(fd);
    }

    if ( fs_cd("/") < 0){
        printf("cd root error!\n");
        return -1;
    }
    
    // then remove /x
    if(fs_rmdir("/x") < 0){
        printf("remove dir /x error!\n");
        return -1;
    }

    //write new file
    //Note: Then test if there is any orphan inode which contains data block.
    
    int max_size = fs_size/8 - other_use -1;//except other blocks
   
    int max_inode = inode - 1;
    for(i=0;i < max_size && i < max_inode;i++){
        char name[3];
        itoa(i,name);
        if ((fd = fs_open(name, FS_O_RDWR)) < 0){
            printf("create file error!\n");
            return -1;
        }
        if(fs_write(fd, buf, count) <= 0){
            printf("write data error!\n");
            return -1;
        }
        fs_close(fd);
        //
        if((i+1+2) % file_count == 0)
        	max_size--;
    }


    printf("rmdir test pass !\n");
    return 0;
}

int main(int argc,char*argv[])
{	
    if(argc < 7){
        printf("Usage:\n");
        printf("./p6_test <#location:superblock1>\n");
        printf("          <#location:superblock2>\n");
        printf("          <#fs_size>\n");
        printf("          <#max_inode>\n");
		printf("          <#file_count>\n");
		printf("          <#other_use>\n");
        printf("Note:\n");
        printf("     1. the location and_fs_size should extent id,DO NOT change secter size in block.h\n");
        printf("     2. avoid too little max_inode, make it matchable with fs_size better\n");
		printf("     3. file_count are the number of files which 4096KB can contain in a directory");
		printf("     4. other_use are the sum of boot,superblock,backup,inode,bitmap...");
        return -1;
    }
    int sb1,sb2,fs_size,inode,file_count,other_use;
    sb1 = atoi(argv[1]);
    sb2 = atoi(argv[2]);
    fs_size = atoi(argv[3]);
    inode = atoi(argv[4]);
	other_use = atoi(argv[5]);
	file_count = atoi(argv[6]);
    printf("Intput: sb1:%d, sb2:%d fs_size:%d max_inode:%d\n",sb1,sb2,fs_size,inode);

    int result[3] = {-1,-1,-1};
    result[0]=superblock_test(sb1,sb2);
    result[1]=path_lookup_test();
    result[2]=rmdir_test(fs_size,inode, file_count, other_use);

    int i=0;
    int pass=0;
    for(;i<3;i++){
        if(0 == result[i])
            pass++;
    }
    
    printf("PASS %d of 3 TEST\n",pass);

    
	return 0;
}

