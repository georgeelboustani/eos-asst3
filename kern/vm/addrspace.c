/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <elf.h>

#define OFFSET_MASK 0x00000fff
#define FIRST_TABLE_INDEX_MASK 0xffc00000
#define SECOND_TABLE_INDEX_MASK 0x003ff000

#define USER_STACKPAGES 16

/*
 * Region helper functions:
 */
struct region* create_region(vaddr_t vbase, size_t npages,
		int readable, int writeable, int executable);
void add_region(struct addrspace* as, struct region* new_region);
struct region* deep_copy_region(struct region* old);
void destroy_regions(struct addrspace* as, struct region* region);
struct region* retrieve_region(struct addrspace* as, vaddr_t faultaddress);

struct region* create_region(vaddr_t vbase, size_t npages, int readable, int writeable, int executable) {
	struct region* new_region = (struct region*) kmalloc(sizeof(struct region));
	new_region->vbase = vbase;
	new_region->npages = npages;
	new_region->readable = readable;
	new_region->writeable = writeable;
	new_region->executable = executable;
	new_region->next = NULL;

	return new_region;
}

void add_region(struct addrspace* as, struct region* new_region) {
	if (as->first_region == NULL) {
		as->first_region = new_region;
	} else {
		struct region* curr_region = as->first_region;
		while (curr_region->next != NULL) {
			curr_region = curr_region->next;
		}
		curr_region->next = new_region;
	}
}

struct region* deep_copy_region(struct region* old) {
	if (old != NULL) {
		struct region* new = (struct region*) kmalloc(sizeof(struct region));
		new->vbase = old->vbase;
		new->npages = old->npages;
		new->readable = old->readable;
		new->writeable = old->writeable;
		new->executable = old->executable;
		new->next = deep_copy_region(old->next);
		return new;
	} else {
		return NULL;
	}
}

void destroy_regions(struct addrspace* as, struct region* region) {
	if (region != NULL) {
		as->num_regions--;
		destroy_regions(as, region->next);
		kfree(region);
	}
}

struct region* retrieve_region(struct addrspace* as, vaddr_t faultaddress) {
	struct region* curr_region = as->first_region;
	while (curr_region != NULL) {
		if (faultaddress >= curr_region->vbase &&
			(faultaddress < curr_region->vbase + curr_region->npages * PAGE_SIZE)) {
			KASSERT(curr_region->vbase != 0);
			KASSERT(curr_region->npages != 0);
			KASSERT((curr_region->vbase & PAGE_FRAME) == curr_region->vbase);
			return curr_region;
		}
		curr_region = curr_region->next;
	}
	return NULL;
}

/*
 * Page table helper functions:
 */
struct page_table_entry* create_page_table(paddr_t pbase, int is_dirty, int is_valid, int index, int offset) {
	struct page_table_entry* new_pte = (struct page_table_entry*) kmalloc(sizeof(struct page_table_entry));
	new_pte->pbase = pbase;
	new_pte->is_dirty = is_dirty;
	new_pte->is_valid = is_valid;
	new_pte->index = index;
	new_pte->offset = offset;
	new_pte->next = NULL;

	return new_pte;
}

struct page_table_entry* add_page_table_entry(struct page_table_entry* head, struct page_table_entry* new_page_table_entry) {
	if (head == NULL) {
		return new_page_table_entry;
	} else {
		struct page_table_entry* curr = head;
		struct page_table_entry* prev = NULL;
		while (curr != NULL && (curr->index < new_page_table_entry->index)) {
			prev = curr;
			curr = curr->next;
		}
		if (prev == NULL) {
			new_page_table_entry->next = head;
			return new_page_table_entry;
		}
		prev->next = new_page_table_entry;
		new_page_table_entry->next = curr;
		return head;
	}
}

struct page_table_entry* deep_copy_page_table(struct page_table_entry* old) {
	if (old != NULL) {
		struct page_table_entry* new_pte = (struct page_table_entry*) kmalloc(sizeof(struct page_table_entry));
		if (new_pte == NULL) {
			return NULL;
		}
		new_pte->index = old->index;
		new_pte->is_dirty = old->is_dirty;
		new_pte->is_valid = old->is_valid;
		new_pte->offset = old->offset;
		new_pte->pbase = old->pbase;
		new_pte->next = deep_copy_page_table(old->next);
		return new_pte;
	} else {
		return NULL;
	}
}

struct page_table_entry* destroy_page_table_entry(struct page_table_entry* head, int index) {
	if (head == NULL) {
		return NULL;
	} else {
		struct page_table_entry* curr = head;
		struct page_table_entry* prev = NULL;
		int found = 0;
		// TODO - not sure why the second check, early exit?
		while (curr != NULL && (curr->index <= index)) {
			if (curr->index == index) {
				found = 1;
				break;
			}
			prev = curr;
			curr = curr->next;
		}

		if (!found) {
			return head;
		}

		if (prev == NULL) {
			struct page_table_entry* new_next = curr->next;
			kfree(curr);
			return new_next;
		}
		prev->next = curr->next;
		kfree(curr);
		return head;
	}
}

struct page_table_entry* page_walk(vaddr_t vaddr, struct addrspace* as, int create_flag) {
	int first_index = (vaddr & FIRST_TABLE_INDEX_MASK) >> 22;
	// TODO - check is second_index still growing by 4's?
	int second_index = ((vaddr & SECOND_TABLE_INDEX_MASK) >> 12);
	size_t offset = vaddr & OFFSET_MASK;

	struct page_table_entry* current_table_entry = as->page_directory[first_index];
	struct page_table_entry* previous_table_entry = NULL;
	while (current_table_entry != NULL) {
		if (current_table_entry->index == second_index) {
			return current_table_entry;
		}

		previous_table_entry = current_table_entry;
		current_table_entry = current_table_entry->next;
	}

	// We didn't find an existing page entry
	if (create_flag) {
		paddr_t page_location = getppages(1);
		KASSERT(page_location != 0);
		KASSERT((page_location & PAGE_FRAME) == page_location);

		struct page_table_entry* new_pte = create_page_table(page_location, 1, 1, second_index, offset);
		
		// TODO - this KASSERT is defensive, is it necessary
		KASSERT((new_pte->pbase & PAGE_FRAME) == new_pte->pbase);
		as->page_directory[first_index] = add_page_table_entry(as->page_directory[first_index], new_pte);
		return new_pte;
	}

	return NULL;
}

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}
	struct page_table_entry** page_directory = (struct page_table_entry**)kmalloc(sizeof(struct page_table_entry*) * PAGE_TABLE_ONE_SIZE);
	if (page_directory == NULL) {
		return NULL;
	}
	as->page_directory = page_directory;

	/*
	 * Initialize as needed.
	 */
	int i = 0;
	while (i < PAGE_TABLE_ONE_SIZE) {
		as->page_directory[i] = NULL;
		i++;
	}

	as->first_region = NULL;
	as->num_regions = 0;

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	newas->first_region = deep_copy_region(old->first_region);

	int i = 0;
	while (i < PAGE_TABLE_ONE_SIZE) {
		// TODO - double check this
		newas->page_directory[i] = deep_copy_page_table(old->page_directory[i]);
		i++;
	}

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	int i = 0;
	while (i < PAGE_TABLE_ONE_SIZE) {
		while (as->page_directory[i] != NULL) {
			struct page_table_entry* pe = as->page_directory[i];
			as->page_directory[i] = pe->next;
			// TODO - we need to get this free working
			kfree((void*)PADDR_TO_KVADDR(pe->pbase));
			kfree(pe);
		}
		i++;
	}

	kfree(as->page_directory);

	destroy_regions(as, as->first_region);

	kfree(as);
}

void
as_activate(void)
{
	int spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
	vm_tlbshootdown_all();
	splx(spl);
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
	int spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
	vm_tlbshootdown_all();

	splx(spl);
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	if (as == NULL) {
		panic("No addrspace provided to define a region in.\n");
	}

	size_t npages;

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	struct region* new_region = create_region(vaddr, npages, readable, writeable, executable);
	add_region(as, new_region);
	as->num_regions++;

	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	as->readonly_preparation = (struct region**)kmalloc(sizeof(struct region*) * as->num_regions);
	if (as->readonly_preparation == NULL) {
		return ENOMEM;
	}

	struct region* current_region = as->first_region;
	int i = 0;
	while (current_region != NULL) {
		if (!current_region->writeable) {
			current_region->writeable = 1;
			as->readonly_preparation[i] = current_region;
			i++;
		}
		current_region = current_region->next;
	}
	// TODO - check why these are is not zeroed by kmalloc?
	while (i < as->num_regions) {
		as->readonly_preparation[i] = NULL;
		i++;
	}

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	int i = 0;
	while (i < as->num_regions) {
		if (as->readonly_preparation[i] != NULL) {
			as->readonly_preparation[i]->writeable = 0;
			i++;
		} else {
			break;
		}
	}

	kfree(as->readonly_preparation);
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/* TODO double check this plz - Initial user-level stack pointer */
	as_define_region(as, USERSTACK - USER_STACKPAGES * PAGE_SIZE, USER_STACKPAGES * PAGE_SIZE, 1, 1, 1);
	*stackptr =  USERSTACK;

	return 0;
}

