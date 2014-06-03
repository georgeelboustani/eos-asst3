#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <proc.h>
#include <current.h>
#include <spl.h>

int clock_hand = 0;

int clock_hand_tlb_knockoff(void);
void write_tlb_entry(vaddr_t faultaddress, paddr_t paddr, uint32_t dirty_bit);

void vm_bootstrap(void)
{
	/* Initialise VM sub-system.  You probably want to initialise your
	frame table here as well.
	*/
	initialize_frame_table();
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	paddr_t paddr;
	struct addrspace *as;

	if (curproc == NULL) {
		 /*
		  * No process. This is probably a kernel fault early
		  * in boot. Return EFAULT so as to panic instead of
		  * getting into an infinite faulting loop.
		  */
		 return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
		 /*
		  * No address space set up. This is probably also a
		  * kernel fault early in boot.
		  */
		return EFAULT;
	}
	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "vm: fault: 0x%x\n", faultaddress);

	struct region* region = retrieve_region(as, faultaddress);
	if (region == NULL) {
		return EFAULT;
	}

	if (region == as->heap) {
		if (faultaddress >= as->heap_end) {
			return EFAULT;
		}
	}

	uint32_t dirty_bit = TLBLO_DIRTY;
	switch (faulttype) {
		case VM_FAULT_READONLY:
			return EFAULT;
		case VM_FAULT_READ:
			// read attempted
			if (!region->readable) {
				return EFAULT;
			}
			break;
		case VM_FAULT_WRITE:
			// write attempted
			if (!region->writeable) {
				return EFAULT;
			}
			break;
		default:
			return EINVAL;
	}

	if (!region->writeable) {
		dirty_bit = 0;
	}

	// Now we know the faultaddress lies within the region
	KASSERT((faultaddress & PAGE_FRAME) == faultaddress);

	struct page_table_entry* page = page_walk(faultaddress, as, 1);
	if (page != NULL) {
		// We found a page mapped to the vaddr.
		paddr = page->pbase;
		
		KASSERT((paddr & PAGE_FRAME) == paddr);
	} else {
		return ENOMEM;
	}

	int spl = splhigh();
	// If we got here then there's no more space in tlb. Knock one off
	write_tlb_entry(faultaddress, paddr, dirty_bit);
	splx(spl);

	return 0;
}

void write_tlb_entry(vaddr_t faultaddress, paddr_t paddr, uint32_t dirty_bit) {	
	int index;
	uint32_t ehi = faultaddress;
	uint32_t elo = paddr | dirty_bit | TLBLO_VALID;

	index = clock_hand_tlb_knockoff();

	DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
	tlb_write(ehi, elo, index);
}

int clock_hand_tlb_knockoff(void) {
	int to_knock = clock_hand;
	clock_hand = (clock_hand + 1) % NUM_TLB;
	return to_knock;
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 *        IMPORTANT NOTE: from tlb_probe :: An entry may be matching even if the valid bit
 *        is not set. To completely invalidate the TLB, load it with
 *        translations for addresses in one of the unmapped address
 *        ranges - these will never be matched.
 */
void
vm_tlbshootdown_all(void)
{
	int i = 0;
	for (i=0; i < NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

