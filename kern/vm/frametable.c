#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <synch.h>

#define SET 1
#define UNSET 0
/* Place your frametable data-structures here 
 * You probably also want to write a frametable initialisation
 * function and call it from vm_bootstrap
 */
struct frame_table_entry {
	// Map a physical address to a virtual address
	struct addrspace* as;
	paddr_t paddr;
	vaddr_t vaddr;

	// Indicate whether or not this frame is taken.
	int free;
	// Indicate if we can ever touch this frame after initializing it.
	int fixed;
};

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
struct frame_table_entry* frame_table = UNSET;
paddr_t free_addr;
struct lock* frame_table_lock;
int total_num_frames;

void initialize_frame_table(void) {
	frame_table_lock = lock_create("frame_table_lock");

	paddr_t paddr_low;
	paddr_t paddr_high;
	ram_getsize(&paddr_low, &paddr_high);

	frame_table = (struct frame_table_entry*) PADDR_TO_KVADDR(paddr_low);

	total_num_frames = (paddr_high - paddr_low) / PAGE_SIZE;
	int size_of_frame_table = total_num_frames * sizeof(struct frame_table_entry);
	free_addr = paddr_low + size_of_frame_table;
	// Align to the next page frame
	free_addr = free_addr + (PAGE_SIZE - (free_addr % PAGE_SIZE));
	
	KASSERT((free_addr % PAGE_SIZE) == 0);
	KASSERT((free_addr & PAGE_FRAME) == free_addr);

	int i = 0;
	// Initialise the frame table, preferrably assign state values
	for (i = 0; i < total_num_frames; i++) {
		frame_table[i].free = SET;
		frame_table[i].fixed = UNSET;
		frame_table[i].paddr = free_addr + i * PAGE_SIZE;
	}
}

paddr_t getppages(unsigned long npages) {
	paddr_t nextfree;

	if (frame_table == UNSET) {
		spinlock_acquire(&stealmem_lock);
		nextfree = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
	} else {
		if (npages > 1) {
			return 0;
		}
		
		lock_acquire(frame_table_lock);
		int i = 0;
		while (frame_table[i].free != SET && i < total_num_frames) {
			i++;
		}

		if (i < total_num_frames) {
			frame_table[i].free = UNSET;
			frame_table[i].fixed = UNSET;
			nextfree = frame_table[i].paddr;
			lock_release(frame_table_lock);
		} else {
			// Out of memory
			lock_release(frame_table_lock);
			return 0;
		}
	}

	bzero((void *)PADDR_TO_KVADDR(nextfree), PAGE_SIZE);

	KASSERT(nextfree % PAGE_SIZE == 0);
	return nextfree;
}

/* Note that this function returns a VIRTUAL address, not a physical 
 * address
 * WARNING: this function gets called very early, before
 * vm_bootstrap().  You may wish to modify main.c to call your
 * frame table initialisation function, or check to see if the
 * frame table has been initialised and call ram_stealmem() otherwise.
 */

vaddr_t alloc_kpages(int npages)
{	
	paddr_t pa = getppages(npages);
	if (pa == 0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void free_kpages(vaddr_t addr)
{
	int freed = UNSET;
	int i = 0;
	while (!freed && i < total_num_frames) {
		if (PADDR_TO_KVADDR(frame_table[i].paddr) == addr) {
			frame_table[i].free = SET;
			frame_table[i].fixed = UNSET;
			freed = SET;
		}
		i++;
	}
}

