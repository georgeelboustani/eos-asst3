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

	// Indicate whether or not this frame is taken.
	int free;
	// Indicate if we can ever touch this frame after initializing it.
	int fixed;
};

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
struct frame_table_entry* frame_table = UNSET;
paddr_t free_addr;
struct lock* frame_table_lock;

void initialize_frame_table(void) {
	frame_table_lock = lock_create("frame_table_lock");

	paddr_t paddr_low;
	paddr_t paddr_high;
	ram_getsize(&paddr_low, &paddr_high);

	frame_table = (struct frame_table_entry*) PADDR_TO_KVADDR(paddr_low);

	int total_num_pages = (paddr_high - paddr_low) / PAGE_SIZE;
	int size_of_frame_table = total_num_pages * sizeof(struct frame_table_entry);
	free_addr = paddr_low + size_of_frame_table;

	lock_acquire(frame_table_lock);
	int i = 0;
	// Initialise the frame table, preferrably assign state values
	for (i = 0; i < total_num_pages; i++) {
		frame_table[i].free = SET;
		frame_table[i].fixed = UNSET;
		frame_table[i].paddr = free_addr + i * PAGE_SIZE;
	}
	lock_release(frame_table_lock);
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
	vaddr_t firstaddr;

	if (frame_table == UNSET) {
		spinlock_acquire(&stealmem_lock);
		firstaddr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
	} else {
		if (npages > 1) {
			return 0;
		}
		
		lock_acquire(frame_table_lock);
		int i = 0;
		while (frame_table[i].free != SET) {
			i++;
		}
		frame_table[i].free = UNSET;
		frame_table[i].fixed = UNSET;
		firstaddr = frame_table[i].paddr;
		lock_release(frame_table_lock);
	}

	bzero((void *)firstaddr, PAGE_SIZE);

	return PADDR_TO_KVADDR(firstaddr);
}

void free_kpages(vaddr_t addr)
{
	// Only good if this page is not mapped to user address space,
	// if as != null we need to unmap the as and shootdown the TLB entry.
	lock_acquire(frame_table_lock);
	int freed = UNSET;
	int i = 0;
	while (!freed) {
		if (PADDR_TO_KVADDR(frame_table[i].paddr) == addr && frame_table[i].as == NULL) {
			frame_table[i].free = SET;
			frame_table[i].fixed = UNSET;
			freed = SET;
		}
		i++;
	}
	lock_release(frame_table_lock);
}

