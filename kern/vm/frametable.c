#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>

#define SET 1
#define UNSET 0
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
struct frame_table_entry* frame_table = UNSET;

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
		frame_table[i].free = UNSET;
		frame_table[i].fixed = SET;
		frame_table[i].vaddr = PADDR_TO_KVADDR(location) + i * PAGE_SIZE;
	}

	// Initialise the rest of the frame table, preferrably assign state values
	for (i = frame_table_pages; i < size_of_frame_table; i++) {
		frame_table[i].as = NULL;
		frame_table[i].free = SET;
		frame_table[i].fixed = UNSET;
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
	vaddr_t firstaddr;

	if (npages > 1) {
		panic("Cannot allocate more than a single page of memory!\n");
	}

	if (frame_table == UNSET) {
		spinlock_acquire(&stealmem_lock);
		firstaddr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
	} else {
		int i = 0;
		while (frame_table[i].free != SET) {
			i++;
		}
		// Need to find a place to set as, only needed if reserving space
		// for a user program.
//		frame_table[i].as = curthread->t_proc->p_addrspace;
		frame_table[i].free = UNSET;
		frame_table[i].fixed = UNSET;
		firstaddr = frame_table[i].vaddr;
	}

	if(firstaddr == 0)
		return 0;

	return firstaddr;
}

void free_kpages(vaddr_t addr)
{
	// Only good if this page is not mapped to user address space,
	// if as != null we need to unmap the as and shootdown the TLB entry.
	int freed = UNSET;
	int i = 0;
	while (!freed) {
		if (frame_table[i].vaddr == addr && frame_table[i].as == NULL) {
			frame_table[i].free = SET;
			frame_table[i].fixed = UNSET;
			freed = SET;
		}
		i++;
	}
}

