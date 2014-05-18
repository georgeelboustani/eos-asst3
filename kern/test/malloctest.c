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

/*
 * Test code for kmalloc.
 */
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <synch.h>
#include <vm.h> /* for PAGE_SIZE */
#include <test.h>

/*
 * Test kmalloc; allocate ITEMSIZE bytes NTRIES times, freeing
 * somewhat later.
 *
 * The total of ITEMSIZE * NTRIES is intended to exceed the size of
 * available memory.
 *
 * mallocstress does the same thing, but from NTHREADS different
 * threads at once.
 */

#define NTRIES   1200
#define ITEMSIZE  997
#define NTHREADS  8

static
void
mallocthread(void *sm, unsigned long num)
{
	struct semaphore *sem = sm;
	void *ptr;
	void *oldptr=NULL;
	void *oldptr2=NULL;
	int i;

	for (i=0; i<NTRIES; i++) {
		ptr = kmalloc(ITEMSIZE);
		if (ptr==NULL) {
			if (sem) {
				kprintf("thread %lu: kmalloc returned NULL\n",
					num);
				V(sem);
				return;
			}
			kprintf("kmalloc returned null; test failed.\n");
			return;
		}
		if (oldptr2) {
			kfree(oldptr2);
		}
		oldptr2 = oldptr;
		oldptr = ptr;
	}
	if (oldptr2) {
		kfree(oldptr2);
	}
	if (oldptr) {
		kfree(oldptr);
	}
	if (sem) {
		V(sem);
	}
}

int
malloctest(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	kprintf("Starting kmalloc test...\n");
	mallocthread(NULL, 0);
	kprintf("kmalloc test done\n");

	return 0;
}

int
mallocstress(int nargs, char **args)
{
	struct semaphore *sem;
	int i, result;

	(void)nargs;
	(void)args;

	sem = sem_create("mallocstress", 0);
	if (sem == NULL) {
		panic("mallocstress: sem_create failed\n");
	}

	kprintf("Starting kmalloc stress test...\n");

	for (i=0; i<NTHREADS; i++) {
		result = thread_fork("mallocstress", NULL,
				     mallocthread, sem, i);
		if (result) {
			panic("mallocstress: thread_fork failed: %s\n",
			      strerror(result));
		}
	}

	for (i=0; i<NTHREADS; i++) {
		P(sem);
	}

	sem_destroy(sem);
	kprintf("kmalloc stress test done\n");

	return 0;
}

int
malloctest3(int nargs, char **args)
{
	static const unsigned sizes[5] = { 32, 41, 109, 86, 9 };
	unsigned numptrs;
	size_t ptrspace;
	size_t blocksize;
	unsigned numptrblocks;
	void ***ptrblocks;
	unsigned curblock, curpos, cursizeindex, cursize;
	size_t totalsize;
	unsigned i, j;
	unsigned char *ptr;

	if (nargs != 2) {
		kprintf("malloctest3: usage: malloctest3 numobjects\n");
		return EINVAL;
	}
	numptrs = atoi(args[1]);
	ptrspace = numptrs * sizeof(void *);

	/* Use the subpage allocator for the pointer blocks too. */
	blocksize = PAGE_SIZE / 4;
	numptrblocks = DIVROUNDUP(ptrspace, blocksize);

	kprintf("malloctest3: %u objects, %u pointer blocks\n",
		numptrs, numptrblocks);

	ptrblocks = kmalloc(numptrblocks * sizeof(ptrblocks[0]));
	if (ptrblocks == NULL) {
		panic("malloctest3: failed on pointer block array\n");
	}
	for (i=0; i<numptrblocks; i++) {
		ptrblocks[i] = kmalloc(blocksize);
		if (ptrblocks[i] == NULL) {
			panic("malloctest3: failed on pointer block %u\n", i);
		}
	}

	curblock = 0;
	curpos = 0;
	cursizeindex = 0;
	totalsize = 0;
	for (i=0; i<numptrs; i++) {
		cursize = sizes[cursizeindex];
		ptr = kmalloc(cursize);
		if (ptr == NULL) {
			kprintf("malloctest3: failed on object %u size %u\n",
				i, cursize);
			kprintf("malloctest3: pos %u in pointer block %u\n",
				curpos, curblock);
			kprintf("malloctest3: total so far %zu\n", totalsize);
			panic("malloctest3: failed.\n");
		}
		for (j=0; j<cursize; j++) {
			ptr[j] = (unsigned char) i;
		}
		ptrblocks[curblock][curpos] = ptr;
		curpos++;
		if (curpos >= blocksize / sizeof(void *)) {
			curblock++;
			curpos = 0;
		}
		totalsize += cursize;
	}

	kprintf("malloctest3: %zu bytes allocated\n", totalsize);

	curblock = 0;
	curpos = 0;
	cursizeindex = 0;
	for (i=0; i<numptrs; i++) {
		cursize = sizes[cursizeindex];
		ptr = ptrblocks[curblock][curpos];
		KASSERT(ptr != NULL);
		for (j=0; j<cursize; j++) {
			if (ptr[j] == (unsigned char) i) {
				continue;
			}
			kprintf("malloctest3: failed on object %u size %u\n",
				i, cursize);
			kprintf("malloctest3: pos %u in pointer block %u\n",
				curpos, curblock);
			kprintf("malloctest3: at object offset %u\n", j);
			kprintf("malloctest3: expected 0x%x, found 0x%x\n",
				ptr[j], (unsigned char) i);
			panic("malloctest3: failed.\n");
		}
		kfree(ptr);
		curpos++;
		if (curpos >= blocksize / sizeof(void *)) {
			curblock++;
			curpos = 0;
		}
		KASSERT(totalsize > 0);
		totalsize -= cursize;
	}
	KASSERT(totalsize == 0);

	for (i=0; i<numptrblocks; i++) {
		KASSERT(ptrblocks[i] != NULL);
		kfree(ptrblocks[i]);
	}
	kfree(ptrblocks);

	kprintf("malloctest3: passed\n");
	return 0;
}
