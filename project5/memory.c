/* Author(s): <Your name here>
 * Implementation of the memory manager for the kernel.
*/

/* memory.c
 *
 * Note: 
 * There is no separate swap area. When a data page is swapped out, 
 * it is stored in the location it was loaded from in the process' 
 * image. This means it's impossible to start two processes from the 
 * same image without screwing up the running. It also means the 
 * disk image is read once. And that we cannot use the program disk.
 *
 */

#include "common.h"
#include "kernel.h"
#include "scheduler.h"
#include "memory.h"
#include "thread.h"
#include "util.h"
#include "interrupt.h"
#include "tlb.h"
#include "usb/scsi.h"

/* Static global variables */
// Keep track of all pages: their vaddr, status, and other properties
static page_map_entry_t page_map[PAGEABLE_PAGES];

// address of the kernel page directory (shared by all kernel threads)
static uint32_t *kernel_pdir;

// allocate the kernel page tables
static uint32_t *kernel_ptabs[N_KERNEL_PTS];

//other global variables...
static lock_t pflock;
static node_t page_queue_real;
static node_t* page_queue=&page_queue_real;


/* Main API */

/* Use virtual address to get index in page directory. */
uint32_t get_dir_idx(uint32_t vaddr){
	return (vaddr & PAGE_DIRECTORY_MASK) >> PAGE_DIRECTORY_BITS;
}
uint32_t* get_pdir_entry(uint32_t* pdir,uint32_t vaddr)
{
	uint32_t dir_idx = get_dir_idx((uint32_t) vaddr);
	uint32_t dir_entry;
	uint32_t *tab;
	dir_entry = pdir[dir_idx];
	tab = (uint32_t *) (dir_entry & PE_BASE_ADDR_MASK);
	return tab;
}

/* Use virtual address to get index in a page table. */
uint32_t get_tab_idx(uint32_t vaddr){
	return (vaddr & PAGE_TABLE_MASK) >> PAGE_TABLE_BITS;
}

/* TODO: Returns physical address of page number i */
uint32_t* page_addr(int i){
	return (uint32_t *) (MEM_START + i*PAGE_SIZE);
}

/* Set flags in a page table entry to 'mode' */
void set_ptab_entry_flags(uint32_t * pdir, uint32_t vaddr, uint32_t mode){
	uint32_t dir_idx = get_dir_idx((uint32_t) vaddr);
	uint32_t tab_idx = get_tab_idx((uint32_t) vaddr);
	uint32_t dir_entry;
	uint32_t *tab;
	uint32_t entry;

	dir_entry = pdir[dir_idx];
	ASSERT(dir_entry & PE_P); /* dir entry present */
	tab = (uint32_t *) (dir_entry & PE_BASE_ADDR_MASK);
	/* clear table[index] bits 11..0 */
	entry = tab[tab_idx] & PE_BASE_ADDR_MASK;

	/* set table[index] bits 11..0 */
	entry |= mode & ~PE_BASE_ADDR_MASK;
	tab[tab_idx] = entry;

	/* Flush TLB because we just changed a page table entry in memory */
	flush_tlb_entry(vaddr);
}

/* Initialize a page table entry
 *  
 * 'vaddr' is the virtual address which is mapped to the physical
 * address 'paddr'. 'mode' sets bit [12..0] in the page table entry.
 *   
 * If user is nonzero, the page is mapped as accessible from a user
 * application.
 */
void init_ptab_entry(uint32_t * table, uint32_t vaddr,
				 uint32_t paddr, uint32_t mode){
	int index = get_tab_idx(vaddr);
	table[index] =
		(paddr & PE_BASE_ADDR_MASK) | (mode & ~PE_BASE_ADDR_MASK);
	flush_tlb_entry(vaddr);
}

/* Insert a page table entry into the page directory. 
 *   
 * 'mode' sets bit [12..0] in the page table entry.
 */
void insert_ptab_dir(uint32_t * dir, uint32_t *tab, uint32_t vaddr, 
					 uint32_t mode){

	uint32_t access = mode & MODE_MASK;
	int idx = get_dir_idx(vaddr);

	dir[idx] = ((uint32_t)tab & PE_BASE_ADDR_MASK) | access;
}

/* TODO: Allocate a page. Return page index in the page_map directory.
 * 
 * Marks page as pinned if pinned == TRUE. 
 * Swap out a page if no space is available. 
 */
int page_alloc(int pinned){
	// define some local needed variables
	uint32_t free_index;
	// find an availabe physical page
	for (free_index = 0; free_index < PAGEABLE_PAGES; ++free_index)
	{
		if(page_map[free_index].is_available==TRUE)
		{
			break;
		}
	}
	if(free_index>=PAGEABLE_PAGES)
	{
		free_index=page_replacement_policy();//select a victim
		page_swap_out(free_index);
	}
	// initialize a physical page (wirte infomation to page_map)
 	page_map[free_index].is_pinned=(pinned==TRUE)?TRUE:FALSE;
 	if(pinned==FALSE)//pinned page need not to be in the swap queue
 		queue_put(page_queue,(node_t*)&page_map[free_index]);
 	page_map[free_index].is_available=FALSE;
 	page_map[free_index].pid=current_running->pid;
 	page_map[free_index].page_directory=current_running->page_directory;
 	page_map[free_index].swap_loc=current_running->swap_loc;
  	page_map[free_index].vaddr=current_running->fault_addr&PE_BASE_ADDR_MASK;
	//calculate real sectors to swap
	int near_sectors=current_running->swap_size % SECTORS_PER_PAGE;
	int need_sector_num=(current_running->fault_addr - PROCESS_START) / SECTOR_SIZE;
	if(current_running->swap_size- near_sectors <= need_sector_num)
		page_map[free_index].swap_size=near_sectors;
	else
		page_map[free_index].swap_size=SECTORS_PER_PAGE;
	// zero-out the process page
	bzero((char*)page_addr(free_index),PAGE_SIZE);
	return free_index;
}

/* TODO: Set up kernel memory for kernel threads to run.
 *
 * This method is only called once by _start() in kernel.c, and is only 
 * supposed to set up the page directory and page tables for the kernel.
 */
void init_memory(void){
	// initialize all pageable pages to a default state
	
	lock_init(&pflock);
	queue_init(page_queue);
	int i;
	for(i=0;i<PAGEABLE_PAGES;i++)
	{
		page_map[i].is_available = TRUE;
	    page_map[i].is_pinned = FALSE;
	    page_map[i].pid = -2;
	    page_map[i].index = i;
	    page_map[i].page_directory=NULL;
  	}
	// pin one page for the kernel page directory
  	page_map[0].is_pinned=TRUE;
	page_map[0].is_available=FALSE;
	page_map[0].pid=-1;
	kernel_pdir=page_addr(0);
	page_map[0].page_directory=kernel_pdir;

	// zero-out the kernel page directory
	bzero((char*)kernel_pdir,PAGE_SIZE);
	// pin N_KERNEL_PTS pages for kernel page tables
	int kernel_vaddr=0;
	for(i=1;i<=N_KERNEL_PTS;i++)
	{
		if(i>=PAGEABLE_PAGES)
			break;
		page_map[i].vaddr=0;
		page_map[i].is_available=FALSE;
		page_map[i].is_pinned=TRUE;
		page_map[i].pid=0;
		kernel_ptabs[i-1]=page_addr(i);
		insert_ptab_dir(kernel_pdir,kernel_ptabs[i-1],kernel_vaddr,PE_P|PE_RW);
		int j;
		for(j=0;j<PAGE_N_ENTRIES;j++)
		{
			if(kernel_vaddr>=MAX_PHYSICAL_MEMORY)
				break;
			init_ptab_entry(kernel_ptabs[i-1],kernel_vaddr,kernel_vaddr,PE_P|PE_RW);
			kernel_vaddr+=PAGE_SIZE;
		}
	}
	set_ptab_entry_flags(kernel_pdir,SCREEN_ADDR,PE_RW|PE_P|PE_US);
}

/* TODO: Set up a page directory and page table for a new 
 * user process or thread. */
void setup_page_table(pcb_t * p){
	// special case for thread virtual memory setup
	if(p->is_thread==TRUE)
	{
		p->page_directory=kernel_pdir;
		return ;
	}
	//set the directory
	uint32_t proc_dir_index=page_alloc(TRUE);
	p->page_directory=page_addr(proc_dir_index);
	page_map[proc_dir_index].pid=p->pid;
	page_map[proc_dir_index].page_directory=p->page_directory;
	bzero((char*)p->page_directory,PAGE_SIZE);
	//set the stack
	uint32_t tindex,pindex;
	uint32_t stack_top=p->user_stack;
	int i=0;
	int j;
	while(i<N_PROCESS_STACK_PAGES){
		tindex=page_alloc(TRUE);
		insert_ptab_dir(p->page_directory,page_addr(tindex),stack_top,PE_RW|PE_P|PE_US);
		page_map[tindex].page_directory=p->page_directory;
		for(j=get_tab_idx(stack_top);j>=0;j--)
		{
			if(i>=N_PROCESS_STACK_PAGES)
				break;
			pindex=page_alloc(TRUE);
			init_ptab_entry(page_addr(tindex),stack_top,page_addr(pindex),PE_RW|PE_P|PE_US);
			page_map[pindex].page_directory=p->page_directory;
			stack_top-=PAGE_SIZE;
			i++;//success alloc one stack page
		}
	}
	//set process table and pages
	uint32_t pstart=p->start_pc;
	uint32_t process_end=pstart+p->swap_size*SECTOR_SIZE;
	while(pstart<process_end){
		tindex=page_alloc(TRUE);
		insert_ptab_dir(p->page_directory,page_addr(tindex),pstart,PE_RW|PE_P|PE_US);
		page_map[tindex].page_directory=p->page_directory;
		for(j=get_tab_idx(pstart);j<PAGE_N_ENTRIES;j++)
		{
			if(pstart>=process_end)
				break;
			init_ptab_entry(page_addr(tindex),pstart,pstart,PE_RW | PE_US);
			pstart+=PAGE_SIZE;//success alloc one proc page
		}
	}
	//set up kernel map
	for(i=0;i<N_KERNEL_PTS;i++)
		insert_ptab_dir(p->page_directory,kernel_ptabs[i],i*PAGE_SIZE*PAGE_N_ENTRIES,PE_P|PE_RW|PE_US);//why PE_US ????
}

/* TODO: Swap into a free page upon a page fault.
 * This method is called from interrupt.c: exception_14(). 
 * Should handle demand paging.
 */
void page_fault_handler(void){
	lock_acquire(&pflock);
	current_running->page_fault_count++;

	uint32_t i=page_alloc(FALSE);
	page_swap_in(i);

	lock_release(&pflock);
}

/* Get the sector number on disk of a process image
 * Used for page swapping. */
int get_disk_sector(page_map_entry_t * page){
	return page->swap_loc +
		((page->vaddr - PROCESS_START) / PAGE_SIZE) * SECTORS_PER_PAGE;
}

/* TODO: Swap from disk into the i-th page using fault address and swap_loc of current running */
void page_swap_in(int i){
	scsi_read(get_disk_sector(&page_map[i]),page_map[i].swap_size,(char *)page_addr(i));
	uint32_t* table_ptr= get_pdir_entry(page_map[i].page_directory,page_map[i].vaddr);
	init_ptab_entry(table_ptr,page_map[i].vaddr,page_addr(i),PE_P|PE_RW|PE_US);
}

uint32_t get_ptab_entry(uint32_t * pdir, uint32_t vaddr) {
	uint32_t dir_idx = get_dir_idx((uint32_t) vaddr);
	uint32_t tab_idx = get_tab_idx((uint32_t) vaddr);
	uint32_t dir_entry;
	uint32_t *tab;
//	uint32_t entry;

	dir_entry = pdir[dir_idx];

	tab = (uint32_t *) (dir_entry & PE_BASE_ADDR_MASK);

	return tab[tab_idx];
}

/* TODO: Swap i-th page out to disk.
 *   
 * Write the page back to the process image.
 * There is no separate swap space on the USB.
 * 
 */
void page_swap_out(int i){
	uint32_t* table_ptr= get_pdir_entry(page_map[i].page_directory,page_map[i].vaddr);
	uint32_t tab_entry = table_ptr[get_tab_idx(page_map[i].vaddr)];

	set_ptab_entry_flags(page_map[i].page_directory,page_map[i].vaddr,PE_US|PE_RW);
	if(tab_entry & PE_D)
		scsi_write(get_disk_sector(&page_map[i]),page_map[i].swap_size,(char*)page_addr(i));
}

/* TODO: Decide which page to replace, return the page number  */
int page_replacement_policy(void){
	ASSERT(queue_empty(page_queue)==FALSE);
	page_map_entry_t* victim=(page_map_entry_t*)queue_get(page_queue);
	ASSERT(victim->is_pinned==FALSE);
	ASSERT(victim->is_available==FALSE);
	return victim->index;
}
