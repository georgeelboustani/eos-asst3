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
 * File-related system call implementations.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/seek.h>
#include <kern/stat.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>

//TODO MOVE DIS SHIET YO
#include <addrspace.h>

/*
 * mk_useruio
 * sets up the uio for a USERSPACE transfer. 
 */
static
void
mk_useruio(struct iovec *iov, struct uio *u,
	   userptr_t buf, size_t len, off_t offset, enum uio_rw rw)
{
	DEBUGASSERT(iov);
	DEBUGASSERT(u);

	iov->iov_ubase = buf;
	iov->iov_len = len;
	u->uio_iov = iov;
	u->uio_iovcnt = 1;
	u->uio_offset = offset;
	u->uio_resid = len;
	u->uio_segflg = UIO_USERSPACE;
	u->uio_rw = rw;
	u->uio_space = proc_getas();
}

/*
 * sys_open
 * just copies in the filename, then passes work to file_open.
 */
int
sys_open(userptr_t filename, int flags, int mode, int *retval)
{
	char fname[PATH_MAX];
	int result;

	result = copyinstr(filename, fname, sizeof(fname), NULL);
	if (result) {
		return result;
	}

	return file_open(fname, flags, mode, retval);
}

/*
 * sys_read
 * translates the fd into its openfile, then calls VOP_READ.
 */
int
sys_read(int fd, userptr_t buf, size_t size, int *retval)
{
	struct iovec iov;
	struct uio useruio;
	struct openfile *file;
	int result;

	/* better be a valid file descriptor */
	result = filetable_findfile(fd, &file);
	if (result) {
		return result;
	}

	lock_acquire(file->of_lock);

	if (file->of_accmode == O_WRONLY) {
		lock_release(file->of_lock);
		return EBADF;
	}

	/* set up a uio with the buffer, its size, and the current offset */
	mk_useruio(&iov, &useruio, buf, size, file->of_offset, UIO_READ);

	/* does the read */
	result = VOP_READ(file->of_vnode, &useruio);
	if (result) {
		lock_release(file->of_lock);
		return result;
	}

	/* set the offset to the updated offset in the uio */
	file->of_offset = useruio.uio_offset;

	lock_release(file->of_lock);
	
	/*
	 * The amount read is the size of the buffer originally, minus
	 * how much is left in it.
	 */
	*retval = size - useruio.uio_resid;

	return 0;
}

/*
 * sys_write
 * translates the fd into its openfile, then calls VOP_WRITE.
 */
int
sys_write(int fd, userptr_t buf, size_t size, int *retval)
{
	struct iovec iov;
	struct uio useruio;
	struct openfile *file;
	int result;

	result = filetable_findfile(fd, &file);
	if (result) {
		return result;
	}

	lock_acquire(file->of_lock);

	if (file->of_accmode == O_RDONLY) {
		lock_release(file->of_lock);
		return EBADF;
	}

	/* set up a uio with the buffer, its size, and the current offset */
	mk_useruio(&iov, &useruio, buf, size, file->of_offset, UIO_WRITE);

	/* does the write */
	result = VOP_WRITE(file->of_vnode, &useruio);
	if (result) {
		lock_release(file->of_lock);
		return result;
	}

	/* set the offset to the updated offset in the uio */
	file->of_offset = useruio.uio_offset;

	lock_release(file->of_lock);

	/*
	 * the amount written is the size of the buffer originally,
	 * minus how much is left in it.
	 */
	*retval = size - useruio.uio_resid;

	return 0;
}

/* 
 * sys_close
 * just pass off the work to file_close.
 */
int
sys_close(int fd)
{
	return file_close(fd);
}

/*
 * sys_lseek
 * translates the fd into its openfile, then based on the type of seek,
 * figure out the new offset, try the seek, if that succeeds, update the
 * openfile.
 */
int
sys_lseek(int fd, off_t offset, int whence, off_t *retval)
{
	struct stat info;
	struct openfile *file;
	int result;

	result = filetable_findfile(fd, &file);
	if (result) {
		return result;
	}

	lock_acquire(file->of_lock);
	
	/* based on the type of seek, set the retval */ 
	switch (whence) {
	    case SEEK_SET:
		*retval = offset;
		break;
	    case SEEK_CUR:
		*retval = file->of_offset + offset;
		break;
	    case SEEK_END:
		result = VOP_STAT(file->of_vnode, &info);
		if (result) {
			lock_release(file->of_lock);
			return result;
		}
		*retval = info.st_size + offset;
		break;
	    default:
		lock_release(file->of_lock);
		return EINVAL;
	}

	/* try the seek -- if it fails, return */
	result = VOP_TRYSEEK(file->of_vnode, *retval);
	if (result) {
		lock_release(file->of_lock);
		return result;
	}
	
	/* success -- update the file structure */
	file->of_offset = *retval;

	lock_release(file->of_lock);

	return 0;
}

/* 
 * sys_dup2
 * just pass the work off to the filetable
 */
int
sys_dup2(int oldfd, int newfd, int *retval)
{
	int result;

	result = filetable_dup2file(oldfd, newfd);
	if (result) {
		return result;
	}

	*retval = newfd;
	return 0;
}

/* really not "file" calls, per se, but might as well put it here */

/*
 * sys_chdir
 * copyin the path and pass it off to vfs.
 */
int
sys_chdir(userptr_t path)
{
	char pathbuf[PATH_MAX];
	int result;
	
	result = copyinstr(path, pathbuf, PATH_MAX, NULL);
	if (result) {
		return result;
	}

	return vfs_chdir(pathbuf);
}

/*
 * sys___getcwd
 * just use vfs_getcwd.
 */
int
sys___getcwd(userptr_t buf, size_t buflen, int *retval)
{
	struct iovec iov;
	struct uio useruio;
	int result;

	mk_useruio(&iov, &useruio, buf, buflen, 0, UIO_READ);

	result = vfs_getcwd(&useruio);
	if (result) {
		return result;
	}

	*retval = buflen - useruio.uio_resid;

	return 0;
}

// TODO: Move this into its own file
int sys_sbrk(int increment, int *retval) {
	struct addrspace* cur_as = curthread->t_proc->p_addrspace;

	KASSERT(cur_as != NULL);
	KASSERT(cur_as->heap != NULL);

	struct region* heap = cur_as->heap;
	vaddr_t old_heap_end = cur_as->heap_end;

	if (increment == 0) {
		*retval =  old_heap_end;
		return 0;
	}

	// TODO - Abert what is this for?
	// Align the increment by 4, increment heap if possible.
	// int remainder = increment % 4;
	// increment += (4-remainder);

	vaddr_t new_heap_end = old_heap_end + increment;

	vaddr_t page_aligned_end = new_heap_end + (PAGE_SIZE - (new_heap_end % PAGE_SIZE));

	if (new_heap_end < heap->vbase || page_aligned_end >= USERSTACK - USER_STACKPAGES * PAGE_SIZE) {
		// Too negative crossing into the previous region, or 	
		// Too high eating into the stack. We check the next page aligned, to be extra defensive
		*retval = -1;
		return EINVAL;
	}

	size_t new_num_pages = ((new_heap_end - heap->vbase) / PAGE_SIZE) + 1;

//	if (to_allocate < 0) {
//		// TODO - do we free or wut
//	} else {
//		int i = 0;
//		while (i < to_allocate) {
//			vaddr_t page_start_address = old_heap_end + (PAGE_SIZE - (old_heap_end % PAGE_SIZE)) + i * PAGE_SIZE;
//
//			KASSERT(page_start_address % PAGE_SIZE == 0);
//			KASSERT((page_start_address & PAGE_FRAME) == page_start_address);
//
//			struct page_table_entry* page = page_walk(page_start_address, cur_as, 1);
//			if (page == NULL) {
//				*retval = -1;
//				return ENOMEM;
//			}
//
//			paddr_t paddr = page->pbase;
//			KASSERT((paddr & PAGE_FRAME) == paddr);
//
//			i++;
//		}
//	}

	cur_as->heap_end = new_heap_end;
	heap->npages = new_num_pages;

	*retval = old_heap_end;
	return 0;
}
