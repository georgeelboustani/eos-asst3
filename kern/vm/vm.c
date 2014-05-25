#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <proc.h>
#include <current.h>

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
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
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
		/* make sure it's page-aligned */
		KASSERT((paddr & PAGE_FRAME) == paddr);
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	//splx(spl);
	return EFAULT;
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */
void
vm_tlbshootdown_all(void)
{
	panic("vm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

