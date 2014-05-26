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
void write_tlb_entry(vaddr_t faultaddress, paddr_t paddr, int index);

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
	//vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	struct addrspace *as;
	int spl;


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
		panic("Cannot retrieve associated region\n");
	}

	switch (faulttype) {
		case VM_FAULT_READONLY:
			panic("Write attempted on read section\n");
			break;
		case VM_FAULT_READ:
			// read attempted
			if (!region->readable) {
				panic("Attempted to read an unreadable region\n");
			}
			break;
		case VM_FAULT_WRITE:
			// write attempted
			if (!region->writeable) {
				panic("Attempted to write an unwriteable region\n");
			}
			break;
		default:
			return EINVAL;
	}

//     vbase1 = as->as_vbase1;
//     vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
//     vbase2 = as->as_vbase2;
//     vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
//     stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
//     stacktop = USERSTACK;
//
//     if (faultaddress >= vbase1 && faultaddress < vtop1) {
//         paddr = (faultaddress - vbase1) + as->as_pbase1;
//     }
//     else if (faultaddress >= vbase2 && faultaddress < vtop2) {
//         paddr = (faultaddress - vbase2) + as->as_pbase2;
//     }
//     else if (faultaddress >= stackbase && faultaddress < stacktop) {
//         paddr = (faultaddress - stackbase) + as->as_stackpbase;
//     }
//     else {
//         return EFAULT;
//     }

	struct page_table_entry* page = page_walk(faultaddress, as, 1);
	if (page != NULL) {
		// We found a page mapped to the vaddr.
		paddr = (faultaddress - region->vbase) + page->pbase;
		/* TODO - is it necessary to make sure it's page-aligned */
		//KASSERT((paddr & PAGE_FRAME) == paddr);
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();


	uint32_t ehi, elo;

	// TODO - fix this motherfucking
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		write_tlb_entry(faultaddress, paddr, i);
		splx(spl);
		return 0;
	}

	// If we got here then there's no more space in tlb. Knock one off
	int index = clock_hand_tlb_knockoff();
	write_tlb_entry(faultaddress, paddr, index);
	splx(spl);
	return 0;

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	//splx(spl);
	return EFAULT;
}

void write_tlb_entry(vaddr_t faultaddress, paddr_t paddr, int index) {
	uint32_t ehi = faultaddress;
	uint32_t elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
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
	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	//panic("vm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

