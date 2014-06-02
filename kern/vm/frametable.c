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

	// Indicate whether or not this frame is taken.
	int free;
	int frame_id;
};

struct free_list_node {
	struct frame_table_entry* frame;
	struct free_list_node* next;
};

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
struct frame_table_entry* frame_table = UNSET;
paddr_t free_addr;
struct lock* frame_table_lock;
int total_num_frames;
struct free_list_node* first_free_frame = NULL;

void initialize_frame_table(void) {
	frame_table_lock = lock_create("frame_table_lock");

	paddr_t paddr_low;
	paddr_t paddr_high;
	ram_getsize(&paddr_low, &paddr_high);

	// Align the free_frame_table to the next page
	paddr_low = paddr_low + (PAGE_SIZE - (paddr_low % PAGE_SIZE));
	frame_table = (struct frame_table_entry*) PADDR_TO_KVADDR(paddr_low);

	int kernel_allocated_frames = paddr_low / PAGE_SIZE;

	total_num_frames = paddr_high / PAGE_SIZE;
	int size_of_frame_table = total_num_frames * sizeof(struct frame_table_entry);
	
	free_addr = paddr_low + size_of_frame_table;
	// Align to the next page frame
	free_addr = free_addr + (PAGE_SIZE - (free_addr % PAGE_SIZE));
	
	KASSERT((free_addr % PAGE_SIZE) == 0);
	KASSERT((free_addr & PAGE_FRAME) == free_addr);

	// How many frames the frame_table will take up
	// TODO - this is page aligned ye?
	int frame_table_frames_needed = (free_addr - paddr_low) / PAGE_SIZE;

	int i = 0;
	// Initialise the frame table, preferrably assign state values
	for (i = 0; i < kernel_allocated_frames + frame_table_frames_needed; i++) {
		frame_table[i].free = UNSET;
		frame_table[i].frame_id = i;
	}

	// Initialise the frame table, and free list
	struct free_list_node* previous_free_list_node = NULL;
	for (; i < total_num_frames; i++) {
		frame_table[i].free = SET;
		frame_table[i].frame_id = i;

		struct free_list_node* current_free_frame = (struct free_list_node*)PADDR_TO_KVADDR(i * PAGE_SIZE);

		if (previous_free_list_node == NULL) {
			first_free_frame = current_free_frame;
		} else {
			previous_free_list_node->next = current_free_frame;
		}

		current_free_frame->frame = &frame_table[i];
		current_free_frame->next = NULL;
		previous_free_list_node = current_free_frame;
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
		
		// lock_acquire(frame_table_lock);
		// int i = 0;
		// while (frame_table[i].free != SET && i < total_num_frames) {
		// 	i++;
		// }

		// if (i < total_num_frames) {
		// 	frame_table[i].free = UNSET;
		// 	nextfree = i * PAGE_SIZE;
		// 	lock_release(frame_table_lock);
		// } else {
		// 	// Out of memory
		// 	lock_release(frame_table_lock);
		// 	return 0;
		// }

		lock_acquire(frame_table_lock);
		
		if (first_free_frame == NULL) {
			lock_release(frame_table_lock);
			return 0;
		}

		struct free_list_node* taken_free_node = first_free_frame;
		taken_free_node->frame->free = UNSET;

		nextfree = taken_free_node->frame->frame_id * PAGE_SIZE;

		first_free_frame = first_free_frame->next;
		lock_release(frame_table_lock);
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
		if (PADDR_TO_KVADDR(i * PAGE_SIZE) == addr) {
			frame_table[i].free = SET;

			struct free_list_node* previous_free_node = first_free_frame;

			first_free_frame = (struct free_list_node*)PADDR_TO_KVADDR(i * PAGE_SIZE);
			// TODO - check this &
			first_free_frame->frame = &frame_table[i];
			first_free_frame->next = previous_free_node;

			freed = SET;
		}
		i++;
	}
}

