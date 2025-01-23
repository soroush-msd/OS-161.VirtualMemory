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
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <elf.h>
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

	// mallocing the address space
	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	// initially everything is NULL inside the address space
	as->regions = NULL;
	as->stackptr = NULL;
	as->pagetable = create_pagetable();

	if (as->pagetable == NULL) {
		return NULL;
	}

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	// create a new address space for the new process
	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	// allocate memory for new regions
	struct region *old_region = old->regions;
	while (old_region != NULL) {
		struct region *new_region = kmalloc(sizeof(struct region));
		if (new_region == NULL) {
			return ENOMEM;
		}

		// assign the values of old region to the newly allocated ones
		new_region->baseaddr = old_region->baseaddr;
		new_region->size = old_region->size;
		new_region->valid_p = old_region->valid_p;
		new_region->temp_p = old_region-> temp_p;
		new_region->next = newas->regions;
		newas->regions = new_region;

		old_region = old_region->next;
	}

	newas->stackptr = old->stackptr;

	// copy the content of each page accross to the new page table
	for (int i = 0; i < 2048; i++) {

		// if the first level node exists, go the second level nodes
		if (old->pagetable[i] != NULL) {
			for (int j = 0; j < 512; j++) {

				// if the physical address stored in the page table is not zero
				if (old->pagetable[i][j] != 0) {

					// allocate new frames, zero them out and copy across data with memmove
					vaddr_t old_address = PADDR_TO_KVADDR(old->pagetable[i][j] & TLBLO_PPAGE); // get the virtual address corresponding to the physical frame
					vaddr_t new_address = alloc_kpages(1); // allocate a new frame
					if(new_address == 0) { return ENOMEM; }
					memmove((void *)new_address, (const void *)old_address, PAGE_SIZE); // copy the contents of the old memory frame into the new memory frame

					// get the physical address of the newly allocated frame
					uint32_t entrylo = KVADDR_TO_PADDR(new_address);
					entrylo |= (old->pagetable[i][j] & TLBLO_VALID); // set the valid bit
					entrylo |= (old->pagetable[i][j] & TLBLO_DIRTY); // set the dirty bit

					// allocate a second level page table if needed
					if (newas->pagetable[i] == NULL) {
						newas->pagetable[i] = kmalloc(512 * sizeof(uint32_t));
						for (int k = 0; k < 512; k++) {
							newas->pagetable[i][k] = 0;
						}
					}
					newas->pagetable[i][j] = entrylo; // store entrylo in the page table
				}
			}
		}
	}

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	// loop through every valid region and free its memory
	while (as->regions != NULL) {
		struct region *to_delete = as->regions;
		as->regions = as->regions->next;
		kfree(to_delete);
	}

	// free the memory for page table
	destroy_pagetable(as->pagetable);

	// free the address space
	kfree(as);
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	int spl = splhigh();

	for (int i=0; i<NUM_TLB; i++) {
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
	 struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	int spl = splhigh();

	for (int i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

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
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	// allocate memory for a new region
	struct region *new = kmalloc(sizeof(struct region));
	if (new == NULL) {
		return ENOMEM;
	}

	// assign its corresponding values
	new->baseaddr = vaddr;
	new->size = memsize;
	new->valid_p = readable | writeable | executable;
	new->temp_p = new->valid_p;
	new->next = as->regions;
	as->regions = new;
	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	if(as == NULL) { return EFAULT; }
	if(as->regions == NULL) { return EFAULT; }

	// change permissions for all regions to rwx
	struct region *elf_region = as->regions;
	while(elf_region != NULL) {
		elf_region->temp_p = elf_region->valid_p;
		elf_region->valid_p = PF_R | PF_W | PF_X;		
		elf_region = elf_region->next;
	}
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	if(as == NULL) { return EFAULT; }
	if(as->regions == NULL) { return EFAULT; }

	// enforce the original permissions for regions
	struct region *elf_region = as->regions;
	while(elf_region != NULL) {
		elf_region->valid_p = elf_region->temp_p;		
		elf_region = elf_region->next;
	}

	// enofrce the dirty bits in pt entries
	// starting from the first level nodes
	for (int i = 0; i < 2048; i++) {

		// if the first level is defined, loop through its second level
		if (as->pagetable[i] != NULL) {
			for (int j = 0; j < 512; j++) {

				// if the the physical address stored is not zero
				if (as->pagetable[i][j] != 0) {

					// retrieve the coressponsing virtual address from i and j
					vaddr_t entry_address = (i << 21) | (j << 12);

					// find the region the virtual address is situated
					struct region *enforced_region = as->regions;
					while(enforced_region != NULL) {
						if((enforced_region->baseaddr <= entry_address) &&
							(entry_address < enforced_region->baseaddr + enforced_region->size)) {

								// obtain the corresponding physical address and set their original permissions
								paddr_t update_entry = (as->pagetable[i][j] & PAGE_FRAME);
								update_entry |= TLBLO_VALID;
								if((enforced_region->valid_p & PF_W) == PF_W) {
									update_entry |= TLBLO_DIRTY;
									as->pagetable[i][j] = update_entry;
								}
								else {
									as->pagetable[i][j] = update_entry;
								}
						}
						enforced_region = enforced_region->next;
					}

				}
			}
		}
	}

	as_activate();
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	
	// allocate memory for stack by defining a new region with read and write permissions
	int result = as_define_region(as, (USERSTACK - (16 * PAGE_SIZE)), (16 * PAGE_SIZE), PF_R, PF_W, 0);
    
	if (result) {
		return result;
	}
	as->stackptr = (vaddr_t *)USERSTACK;
	*stackptr = USERSTACK;

	return 0;

}

