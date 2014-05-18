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
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

#include <limits.h>

struct lock;
struct vnode;


/*** openfile section ***/

/* 
 * openfile struct 
 * note that there's not too much to keep track of, since the vnode does most
 * of that.  note that it does require synchronization, because a single
 * openfile can be shared between processes (filetable inheritance).
 */
struct openfile {
	struct vnode *of_vnode;
	
	struct lock *of_lock;
	off_t of_offset;
	int of_accmode;	/* from open: O_RDONLY, O_WRONLY, or O_RDWR */
	int of_refcount;
};

/* opens a file (must be kernel pointers in the args) */
int file_open(char *filename, int flags, int mode, int *retfd);

/* closes a file */
int file_close(int fd);


/*** file table section ***/

/*
 * filetable struct
 * just an array of open files.  nice and simple.  doesn't require
 * synchronization, because a table can only be owned by a single process (on
 * inheritance in fork, the table is copied).
 */
struct filetable {
	struct openfile *ft_openfiles[OPEN_MAX];
};

/* these all have an implicit arg of the curthread's filetable */
int filetable_init(const char *inpath, const char *outpath, 
		   const char *errpath);
int filetable_copy(struct filetable **copy);
int filetable_placefile(struct openfile *file, int *fd);
int filetable_findfile(int fd, struct openfile **file);
int filetable_dup2file(int oldfd, int newfd);
void filetable_destroy(struct filetable *ft);


#endif /* _FILE_H_ */
