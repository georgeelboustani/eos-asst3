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

#define OFFSET_MASK 0x00000fff
#define FIRST_TABLE_INDEX_MASK 0xffc00000
#define SECOND_TABLE_INDEX_MASK 0x003ff000
/*
 * Region helper functions:
 */
struct region* create_region(vaddr_t vbase, size_t npages,
		int readable, int writeable, int executable);
void add_region(struct addrspace* as, struct region* new_region);
struct region* deep_copy_region(struct region* old);
void destroy_regions(struct region* region);
struct region* retrieve_region(struct addrspace* as, vaddr_t faultaddress);

struct region* create_region(vaddr_t vbase, size_t npages,
		int readable, int writeable, int executable) {
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
		struct region* previous_region = NULL;
		while (curr_region != NULL) {
			previous_region = curr_region;
			curr_region = curr_region->next;
		}
		previous_region->next = new_region;
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

void destroy_regions(struct region* region) {
	if (region != NULL) {
		destroy_regions(region->next);
		kfree(region);
	}
}

struct region* retrieve_region(struct addrspace* as, vaddr_t faultaddress) {
	struct region* curr = as->first_region;
	while (curr != NULL) {
		if (curr->vbase <= faultaddress &&
			(faultaddress <= curr->vbase + curr->npages * PAGE_SIZE)) {
			KASSERT(curr->vbase != 0);
			KASSERT(curr->npages != 0);
			KASSERT((curr->vbase & PAGE_FRAME) == curr->vbase);
//			KASSERT(as->as_stackpbase != 0);
//			KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
			return curr;
		}
		curr = curr->next;
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
		while (curr != NULL && (curr->index != index)) {
			prev = curr;
			curr = curr->next;
			if (curr->index == index) {
				found = 1;
			}
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
	vaddr_t first_index = (vaddr & 0xffc00000) >> 22;
	vaddr_t second_index = (vaddr & 0x003ff000) >> 12;

	if (as->page_directory[first_index][second_index] == NULL) {
		if (create_flag) {
			// TODO FIX THIS? HOW TO GET PADDR?
//			struct page_table_entry* new_pte = create_page_table();
//			return new_pte;
			return NULL;
		} else {
			return NULL;
		}
	} else {
		return as->page_directory[first_index][second_index];
	}
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

	/*
	 * Initialize as needed.
	 */
	int i = 0;
	while (i < PAGE_TABLE_ONE_SIZE) {
		as->page_directory[i] = NULL;
		i++;
	}

	as->first_region = NULL;

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

	//TODO: Deep copy the page table as well.... :(
	int i = 0;
	while (i < PAGE_TABLE_ONE_SIZE) {
		//newas->page_table_one[i] = deep_copy_page_table(old->page_table_one[i]);
		i++;
	}

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	destroy_regions(as->first_region);
	int i = 0;
	while (i < PAGE_TABLE_ONE_SIZE) {
		//destroy_page_table(as->page_table_one[i]);
		i++;
	}
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
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

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

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

	return 0;
// TODO: Consider where else this might be needed.. if ever.
//	int i = 0;
//	if (as->page_table_one[npages] == NULL) {
//		as->page_table_one[npages] = kmalloc(sizeof(struct page_table_entry) * PAGE_TABLE_TWO_SIZE);
//		while (i < PAGE_TABLE_TWO_SIZE) {
//			as->page_table_one[npages][i] = NULL;
//			i++;
//		}
//	}
//	i = 0;
//	while (i < PAGE_TABLE_TWO_SIZE) {
//		if (as->page_table_one[npages][i] == NULL) {
//			struct page_table_entry* pte = kmalloc(sizeof(struct page_table_entry*));
//			pte->as_vbase = vaddr;
//			pte->as_pbase = 0;
//			pte->as_npages = npages;
//			pte->is_dirty = 1;
//			pte->readable = readable;
//			pte->writeable = writeable;
//			pte->executable = executable;
//			as->page_table_one[npages][i] = pte;
//			return 0;
//		}
//		i++;
//	}
}

int
as_prepare_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * Write this.
	 */

	(void)as;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}

