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
#include <elf.h>


/* Place your page table functions here */

/*
    Creates an empty page table. Returns NULL on failure
    The first level of the page table is an array of size 2048 indexed by the first 11
    bits of the virtual page number. Initially it is filled with NULL pointers.
    When we wish to fill it, we create a second level array of size 512 that will be 
    indexed by the last 9 bits of the virtual page number, and filled with entries of the
    form of EntryLo in the TLB, ie
        physical frame number    Not cacheable   Dirty    Valid    Global    0    
               20 bits                1 bit      1 bit    1 bit    1 bit   8 bits  
*/

// allocate memory for a new page table and assign the first level entries to NULL
paddr_t **create_pagetable(void) {
    paddr_t **pagetable = kmalloc(2048 * sizeof(paddr_t *));
    if (pagetable == NULL) {
        return NULL;
    }
    for (int i = 0; i < 2048; i++) {
        pagetable[i] = NULL;
    }
    return pagetable;
}

// free the allocated frames and the page table itself
void destroy_pagetable(paddr_t **pagetable) {
    for (int i = 0; i < 2048; i++) {
        if (pagetable[i] != NULL) {
            for (int j = 0; j < 512; j++) {
                if (pagetable[i][j] != 0) {
                    free_kpages(PADDR_TO_KVADDR(pagetable[i][j] & PAGE_FRAME));
                }
            }
        }
        kfree(pagetable[i]);
    }
    kfree(pagetable);
}

void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *  
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    if(faultaddress == 0) { return EFAULT; }

    vaddr_t raw_address = faultaddress;
    faultaddress &= PAGE_FRAME;
    struct addrspace *as;
    uint32_t ehi, elo;
    int spl;
    
    // based on slide 27 on assignment 3 presentation slides

	switch (faulttype) {
        // if vm fault readonly -> return EFAULT
	    case VM_FAULT_READONLY:
        return EFAULT;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

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

    if (as->regions == NULL) { return EFAULT; }
    
    // lookup page table
    if (as->pagetable == NULL) { return EFAULT; }

    // see if the fault address lies within a valid region and set the region_valid accordingly
    struct region *check_region = as->regions;
    int region_valid = 0;
    while(check_region != NULL) {
        if((check_region->baseaddr <= raw_address) &&
		(raw_address < check_region->baseaddr + check_region->size)) {
            region_valid = 1;
            break;
        }
        check_region = check_region->next;
    }
    
    // acquire the index of first and second level node of the page table from the fault address
    int first_levelPT = faultaddress >> 21;
    int second_levelPT = (faultaddress << 11) >> 23;

    // check if the translation within the page table is valid

    // if the first level exists
    if (as->pagetable[first_levelPT] != NULL) {

        // if the second level is not address 0 and if the region is valid
        if (as->pagetable[first_levelPT][second_levelPT] != 0) {
            if(region_valid) {

                // set entry hi and entry lo accordingly and write to tlb
                ehi = faultaddress;
                elo = as->pagetable[first_levelPT][second_levelPT];
                spl = splhigh();
                tlb_random(ehi, elo);
                splx(spl);
                return 0;
            } 
        }
    }

    // if no valid translation found inside the page table
    //  and if region is invalid:
    if(!region_valid) { return EFAULT; }
    
    // if region is valid, allocate a new frame and zero fill
    vaddr_t new_page = alloc_kpages(1);
    if(new_page == 0) { return ENOMEM; }
    bzero((void*)new_page, 4096);
    paddr_t new_physical = KVADDR_TO_PADDR(new_page);
    
    // set the permissions accordingly
    new_physical |= TLBLO_VALID;
    if((check_region->valid_p & PF_W) == PF_W) {
        new_physical |= TLBLO_DIRTY;
    }

    // insert the translation inside the page table

    
    if (as->pagetable[first_levelPT] != NULL) {
        // if there is already a valid translation within the index return efault
        if(as->pagetable[first_levelPT][second_levelPT] != 0) { return EFAULT; }
        
    }

    // else allocate new seond level nodes and initilise them to 0
    else {
        as->pagetable[first_levelPT] = kmalloc(512 * sizeof(paddr_t));
        if(as->pagetable[first_levelPT] == NULL) { return ENOMEM; }
        for(int i = 0; i < 512; i++) {
            as->pagetable[first_levelPT][i] = 0;
        }
    }

    // insert the newly allocated frame as a page table entry
    as->pagetable[first_levelPT][second_levelPT] = new_physical;

    // set entrylo and entryhi and wrtie to tlb
    ehi = faultaddress;
    elo = as->pagetable[first_levelPT][second_levelPT];
    spl = splhigh();
    tlb_random(ehi, elo);
    splx(spl);
    
    return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

