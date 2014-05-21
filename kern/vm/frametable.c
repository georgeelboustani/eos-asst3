#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>

/* Place your frametable data-structures here 
 * You probably also want to write a frametable initialisation
 * function and call it from vm_bootstrap
 */
struct frame_table_entry {
	// Map a physical address to a virtual address
	struct addrspace* as;
	vaddr_t vaddr;

	// Indicate whether or not this frame is taken.
	int free;
	// Indicate if we can ever touch this frame after initializing it.
	int fixed;
};

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
struct frame_table_entry* frame_table = 0;

void initialize_frame_table(void) {
	paddr_t paddr_low;
	paddr_t paddr_high;
	ram_getsize(&paddr_low, &paddr_high);

	int total_num_pages = paddr_high / PAGE_SIZE;
	int size_of_frame_table = total_num_pages * sizeof(struct frame_table_entry);
	paddr_t location = paddr_high - (size_of_frame_table);
	frame_table = (struct frame_table_entry*) PADDR_TO_KVADDR(location);

	// Number of pages required to accommodate the entire frame table:
	int frame_table_pages = size_of_frame_table / PAGE_SIZE;
	int i = 0;
	for (i = 0; i < frame_table_pages; i++) {
		frame_table[i].as = NULL;
		frame_table[i].free = 0;
		frame_table[i].fixed = 1;
		frame_table[i].vaddr = PADDR_TO_KVADDR(location) + i * PAGE_SIZE;
	}
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
	/*
	 * IMPLEMENT ME.  You should replace this code with a proper implementation.
	 */

	paddr_t addr;

	if (frame_table == 0) {
		spinlock_acquire(&stealmem_lock);
		addr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
	} else {
		// TODO: malloc info properly.
	}

	if(addr == 0)
		return 0;

	return PADDR_TO_KVADDR(addr);
}

void free_kpages(vaddr_t addr)
{
	(void) addr;
}

