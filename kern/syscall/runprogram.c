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
 * Code for running a user program from the menu, and code for execv,
 * which have a lot in common.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <limits.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <file.h>
#include <syscall.h>
#include <test.h>

// XXX shouldn't be here
#define NARG_MAX 1024

/*
 * argvdata structure.
 *
 * This is a single global (and synchronized) temporary storage for
 * argv. We make it global because exec uses a lot of kernel memory;
 * if we didn't restrict things we'd run out pretty rapidly. We bundle
 * things into a structure to make it relatively easy to move to having
 * e.g. two argv buffers instead of one.
 *
 * A better way to do this is to allocate the argv buffer in pageable
 * virtual memory. However, we don't have a VM system for that yet.
 */
struct argvdata {
	char *buffer;
	char *bufend;
	size_t *offsets;
	int nargs;
	struct lock *lock;
};

static struct argvdata argdata;

void
execv_bootstrap(void)
{
	argdata.lock = lock_create("argvlock");
	if (argdata.lock == NULL) {
		panic("Cannot create argv data lock\n");
	}
}


/*
 * Copy an argv array into kernel space, using an argvdata buffer.
 */
static
int
copyin_args(userptr_t argv, struct argvdata *ad)
{
	userptr_t argptr;
	size_t arglen;
	size_t bufsize, bufresid;
	int result;

	KASSERT(lock_do_i_hold(ad->lock));

	/* for convenience */
	bufsize = bufresid = ARG_MAX;

	/* reset the argvdata */
	ad->bufend = ad->buffer;

	/* loop through the argv, grabbing each arg string */
	for (ad->nargs = 0; ad->nargs <= NARG_MAX; ad->nargs++) {

		/* 
		 * first, copyin the pointer at argv 
		 * (shifted at the end of the loop)
		 */ 
		result = copyin(argv, &argptr, sizeof(userptr_t));
		if (result) {
			return result;
		}

		/* if the argptr is NULL, we hit the end of the argv */
		if (argptr == NULL) {
			break;
		}

		/* too many args? bail */
		if (ad->nargs >= NARG_MAX) {
			return E2BIG;
		}

		/* otherwise, copyinstr the arg into the argvdata buffer */
		result = copyinstr(argptr, ad->bufend, bufresid, &arglen);
		if (result == ENAMETOOLONG) {
			return E2BIG;
		}
		else if (result) {
			return result;
		}

		/* got one -- update the argvdata and the local argv userptr */
		ad->offsets[ad->nargs] = bufsize - bufresid;
		ad->bufend += arglen;
		bufresid -= arglen;
		argv += sizeof(userptr_t);
	}

	return 0;
}

/*
 * Copy an argv out of kernel space to user space.
 */
static
int
copyout_args(struct argvdata *ad, userptr_t *argv, vaddr_t *stackptr)
{
	userptr_t argbase, userargv, arg;
	vaddr_t stack;
	size_t buflen;
	int i, result;

	KASSERT(lock_do_i_hold(ad->lock));

	/* we use the buflen a lot, precalc it */
	buflen = ad->bufend - ad->buffer;

	/* begin the stack at the passed in top */
	stack = *stackptr;

	/*
	 * Copy the block of strings to the top of the user stack.
	 * We can do it as one big blob.
	 */

	/* figure out where the strings start */
	stack -= buflen;

	/* align to sizeof(void *) boundary, this is the argbase */
	stack -= (stack & (sizeof(void *) - 1));
	argbase = (userptr_t)stack;

	/* now just copyout the whole block of arg strings  */
	result = copyout(ad->buffer, argbase, buflen);
	if (result) {
		return result;
	}

	/*
	 * now copy out the argv itself.
	 * the stack pointer is already suitably aligned.
	 * allow an extra slot for the NULL that terminates the vector.
	 */
	stack -= (ad->nargs + 1)*sizeof(userptr_t);
	userargv = (userptr_t)stack;

	for (i = 0; i < ad->nargs; i++) {
		arg = argbase + ad->offsets[i];
		result = copyout(&arg, userargv, sizeof(userptr_t));
		if (result) {
			return result;
		}
		userargv += sizeof(userptr_t);
	}

	/* NULL terminate it */
	arg = NULL;
	result = copyout(&arg, userargv, sizeof(userptr_t));
	if (result) {
		return result;
	}

	*argv = (userptr_t)stack;
	*stackptr = stack;
	return 0;
}

/*
 * Common code for execv and runprogram: loading the executable.
 */
static
int
loadexec(char *path, vaddr_t *entrypoint, vaddr_t *stackptr)
{
	struct addrspace *newvm, *oldvm;
	struct vnode *v;
	char *newname;
	int result;

	/* new name for thread */
	newname = kstrdup(path);
	if (newname == NULL) {
		return ENOMEM;
	}

	/* open the file. */
	result = vfs_open(path, O_RDONLY, 0, &v);
	if (result) {
		kfree(newname);
		return result;
	}

	/* make a new address space. */
	newvm = as_create();
	if (newvm == NULL) {
		vfs_close(v);
		kfree(newname);
		return ENOMEM;
	}

	/* replace address spaces, and activate the new one */
	oldvm = proc_setas(newvm);
	as_activate();

 	/*
	 * Load the executable. If it fails, restore the old address
	 * space and (re-)activate it.
	 */
	result = load_elf(v, entrypoint);
	if (result) {
		vfs_close(v);
		proc_setas(oldvm);
		as_activate();
		as_destroy(newvm);
		kfree(newname);
		return result;
	}

	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(newvm, stackptr);
	if (result) {
		proc_setas(oldvm);
		as_activate();
		as_destroy(newvm);
		kfree(newname);
		return result;
        }

	/*
	 * Wipe out old address space.
	 *
	 * Note: once this is done, execv() must not fail, because there's
	 * nothing left for it to return an error to.
	 */
	if (oldvm) {
		as_destroy(oldvm);
	}

	/*
	 * Now that we know we're succeeding, change the current thread's
	 * name to reflect the new process.
	 */
	kfree(curthread->t_name);
	curthread->t_name = newname;

	return 0;
}


/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Opens the standard file descriptors if necessary.
 *
 * Calls vfs_open on PROGNAME (via loadexec) and thus may destroy it,
 * so it needs to be mutable.
 */
int
runprogram(char *progname)
{
	vaddr_t entrypoint, stackptr;
	int argc;
	userptr_t argv;
	int result;

	/* We must be a thread that can run in a user process. */
	KASSERT(curproc->p_pid >= PID_MIN && curproc->p_pid <= PID_MAX);

	/* We should be a new process. */
	KASSERT(proc_getas() == NULL);

	/* Set up stdin/stdout/stderr if necessary. */
	if (curproc->p_filetable == NULL) {
		result = filetable_init("con:", "con:", "con:");
		if (result) {
			return result;
		}
	}

	lock_acquire(argdata.lock);

	/*
	 * Cons up argv.
	 */

	if (strlen(progname) + 1 > ARG_MAX) {
		lock_release(argdata.lock);
		return E2BIG;
	}
	argdata.buffer = kmalloc(strlen(progname) + 1);
	if (argdata.buffer == NULL) {
		lock_release(argdata.lock);
		return ENOMEM;
	}
	argdata.offsets = kmalloc(sizeof(size_t));
	if (argdata.offsets == NULL) {
		kfree(argdata.buffer);
		lock_release(argdata.lock);
		return ENOMEM;
	}
	strcpy(argdata.buffer, progname);
	argdata.bufend = argdata.buffer + (strlen(argdata.buffer) + 1);
	argdata.offsets[0] = 0;
	argdata.nargs = 1;

	/* Load the executable. Note: must not fail after this succeeds. */
	result = loadexec(progname, &entrypoint, &stackptr);
	if (result) {
		kfree(argdata.buffer);
		kfree(argdata.offsets);
		lock_release(argdata.lock);
		return result;
	}

	result = copyout_args(&argdata, &argv, &stackptr);
	if (result) {
		kfree(argdata.buffer);
		kfree(argdata.offsets);
		lock_release(argdata.lock);

		/* If copyout fails, *we* messed up, so panic */
		panic("execv: copyout_args failed: %s\n", strerror(result));
	}

	argc = argdata.nargs;

	/* free the space */
	kfree(argdata.buffer);
	kfree(argdata.offsets);

	lock_release(argdata.lock);

	/* Warp to user mode. */
	enter_new_process(argc, argv, NULL /*env*/, stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

/*
 * execv.
 *
 * 1. Copy in the program name.
 * 2. Copy in the argv with copyin_args.
 * 3. Load the executable.
 * 4. Copy the argv out again with copyout_args.
 * 5. Warp to usermode.
 */
int
sys_execv(userptr_t prog, userptr_t argv)
{
	char *path;
	vaddr_t entrypoint, stackptr;
	int argc;
	int result;

	path = kmalloc(PATH_MAX);
	if (!path) {
		return ENOMEM;
	}

	/* Get the filename. */
	result = copyinstr(prog, path, PATH_MAX, NULL);
	if (result) {
		kfree(path);
		return result;
	}

	/* get the argv strings. */

	lock_acquire(argdata.lock);

	/* allocate space */
	argdata.buffer = kmalloc(ARG_MAX);
	if (argdata.buffer == NULL) {
		lock_release(argdata.lock);
		kfree(path);
		return ENOMEM;
	}
	argdata.offsets = kmalloc(NARG_MAX * sizeof(size_t));
	if (argdata.offsets == NULL) {
		kfree(argdata.buffer);
		lock_release(argdata.lock);
		kfree(path);
		return ENOMEM;
	}

	/* do the copyin */
	result = copyin_args(argv, &argdata);
	if (result) {
		kfree(path);
		kfree(argdata.buffer);
		kfree(argdata.offsets);
		lock_release(argdata.lock);
		return result;
	}

	/* Load the executable. Note: must not fail after this succeeds. */
	result = loadexec(path, &entrypoint, &stackptr);
	if (result) {
		kfree(path);
		kfree(argdata.buffer);
		kfree(argdata.offsets);
		lock_release(argdata.lock);
		return result;
	}

	/* don't need this any more */
	kfree(path);

	/* Send the argv strings to the process. */
	result = copyout_args(&argdata, &argv, &stackptr);
	if (result) {
		lock_release(argdata.lock);

		/* if copyout fails, *we* messed up, so panic */
		panic("execv: copyout_args failed: %s\n", strerror(result));
	}
	argc = argdata.nargs;

	/* free the argdata space */    
	kfree(argdata.buffer);
	kfree(argdata.offsets);

	lock_release(argdata.lock);

	/* Warp to user mode. */
	enter_new_process(argc, argv, NULL /*env*/, stackptr, entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}
