/****************************************************************************

    Serial line filesystem server
    Copyright (C) 2012 Andre Fachat

    Derived from:
    OS/A65 Version 1.3.12
    Multitasking Operating System for 6502 Computers
    Copyright (C) 1989-1997 Andre Fachat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

****************************************************************************/

/*
 * This file is a filesystem provider implementation, to be
 * used with the FSTCP program on an OS/A65 computer. 
 *
 * In this file the actual command work is done for the 
 * local filesystem.
 */
void *dumb_realpath_root_ptr;
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>

// to get a proper definition of realpath()
#define _XOPEN_SOURCE
#define	__USE_XOPEN_EXTENDED
#include <stdlib.h>

#include "dir.h"
#include "fscmd.h"
#include "provider.h"
#include "errors.h"
#include "mem.h"

#include "log.h"

#undef DEBUG_READ

#define	MAX_BUFFER_SIZE	64

//#define	min(a,b)	(((a)<(b))?(a):(b))

typedef struct {
	int		chan;		// channel for which the File is
	FILE		*fp;
	DIR 		*dp;
	char		dirpattern[MAX_BUFFER_SIZE];
	struct dirent	*de;
	unsigned int	is_first :1;	// is first directory entry?
} File;

typedef struct {
	// derived from endpoint_t
	struct provider_t 	*ptype;
	// payload
	char			*basepath;			// malloc'd base path
	char			*curpath;			// malloc'd current path
	File 			files[MAXFILES];
} fs_endpoint_t;

void fsp_init() {
}

extern provider_t fs_provider;

static void init_fp(File *fp) {

	//log_debug("initializing fp=%p (used to be chan %d)\n", fp, fp == NULL ? -1 : fp->chan);

        fp->chan = -1;
	fp->fp = NULL;
	fp->dp = NULL;
}


// close a file descriptor
static int close_fd(File *file) {
	int er = 0;
	if (file->fp != NULL) {
		er = fclose(file->fp);
		if (er < 0) {
			log_errno("Error closing fd");
		}
		file->fp = NULL;
	}
	if (file->dp != NULL) {
		er = closedir(file->dp);
		if (er < 0) {
			log_errno("Error closing dp");
		}
		file->dp = NULL;
	}
	init_fp(file);
	return er;
}


static void fsp_free(endpoint_t *ep) {
        fs_endpoint_t *cep = (fs_endpoint_t*) ep;
        int i;
        for(i=0;i<MAXFILES;i++) {
                close_fd(&(cep->files[i]));
        }
	mem_free(cep->basepath);
	mem_free(cep->curpath);
        mem_free(ep);
}

static endpoint_t *fsp_new(endpoint_t *parent, const char *path) {

	fs_endpoint_t *parentep = (fs_endpoint_t*) parent;

	// alloc and init a new endpoint struct
	fs_endpoint_t *fsep = malloc(sizeof(fs_endpoint_t));
	fsep->basepath = NULL;
	fsep->curpath = NULL;
        for(int i=0;i<MAXFILES;i++) {
		init_fp(&(fsep->files[i]));
        }

	fsep->ptype = (struct provider_t *) &fs_provider;

	char *dirpath = malloc_path((parentep == NULL) ? NULL : parentep->curpath,
				path);

	// malloc's a buffer and stores the canonical real path in it
	fsep->basepath = realpath(dirpath, NULL);

	mem_free(dirpath);

	if (fsep->basepath == NULL) {
		// some problem with dirpath - maybe does not exist...
		log_errno("Could not resolve path for assign");
		fsp_free((endpoint_t*)fsep);
		return NULL;
	}

	if (parentep != NULL) {
		// if we have a parent, make sure we do not 
		// escape the parent container, i.e. basepath
		if (strstr(fsep->basepath, parentep->basepath) != fsep->basepath) {
			// the parent base path is not at the start of the new base path
			// so we throw an error
			log_error("ASSIGN broke out of container (%s), was trying %s\n",
				parentep->basepath, fsep->basepath);
			fsp_free((endpoint_t*)fsep);
				
			return NULL;
		}
	}

	// copy into current path
	fsep->curpath = malloc(strlen(fsep->basepath) + 1);
	strcpy(fsep->curpath, fsep->basepath);

	log_info("FS provider set to real path '%s'\n", fsep->basepath);

	return (endpoint_t*) fsep;
}

// ----------------------------------------------------------------------------------
// error translation

static int errno_to_error(int err) {

	switch(err) {
	case EEXIST:
		return ERROR_FILE_EXISTS;
	case EACCES:
		return ERROR_NO_PERMISSION;
	case ENAMETOOLONG:
		return ERROR_FILE_NAME_TOO_LONG;
	case ENOENT:
		return ERROR_FILE_NOT_FOUND;
	case ENOSPC:
		return ERROR_DISK_FULL;
	case EROFS:
		return ERROR_WRITE_PROTECT;
	case ENOTDIR:	// mkdir, rmdir
	case EISDIR:	// open, rename
		return ERROR_FILE_TYPE_MISMATCH;
	case ENOTEMPTY:
		return ERROR_DIR_NOT_EMPTY;
	case EMFILE:
		return ERROR_NO_CHANNEL;
	case EINVAL:
		return ERROR_SYNTAX_INVAL;
	default:
		return ERROR_FAULT;
	}
}


// ----------------------------------------------------------------------------------
//

static File *reserve_file(endpoint_t *ep, int chan) {
        fs_endpoint_t *cep = (fs_endpoint_t*) ep;

        for (int i = 0; i < MAXFILES; i++) {
                if (cep->files[i].chan == chan) {
                        close_fd(&(cep->files[i]));
                }
                if (cep->files[i].chan < 0) {
                        File *fp = &(cep->files[i]);
                        init_fp(fp);
                        fp->chan = chan;

			//log_debug("reserving file %p for chan %d\n", fp, chan);

                        return &(cep->files[i]);
                }
        }
        log_warn("Did not find free curl session for channel=%d\n", chan);
        return NULL;
}

static File *find_file(endpoint_t *ep, int chan) {
        fs_endpoint_t *cep = (fs_endpoint_t*) ep;

        for (int i = 0; i < MAXFILES; i++) {
                if (cep->files[i].chan == chan) {
                        return &(cep->files[i]);
                }
        }
        log_warn("Did not find curl session for channel=%d\n", chan);
        return NULL;
}

// ----------------------------------------------------------------------------------
// commands as sent from the device

// close a file descriptor
static void close_fds(endpoint_t *ep, int tfd) {
	File *file = find_file(ep, tfd); // ((fs_endpoint_t*)ep)->files;
	if (file != NULL) {
		close_fd(file);
		init_fp(file);
	}
}

static int path_under_base(char *path, char *base) {
/* 
 * Return
 * -3 if malloc() failed
 * -2 if realpath(path) failed
 * -1 if realpath(base) failed
 *  0 if it is
 *  1 if it is not
 */
	int res = 1;
	char *base_realpathc = NULL;
	char *base_dirc = NULL;
	char *path_dname = NULL;
	char *path_realpathc = NULL;

	if(!base) return -1;
	if(!path) return -2;

	base_realpathc = realpath(base, NULL);
	if(!base_realpathc) {
		res = -1;
		log_error("Unable to get real path for '%s'\n", base);
		goto exit;
	}
	base_dirc = mem_alloc_c(strlen(base_realpathc) + 2, "base realpath/");
	strcpy(base_dirc, base_realpathc);
	if(!base_dirc) {
		res = -3;
		goto exit;
	}
	strcat(base_dirc, "/");

	path_realpathc = realpath(path, NULL);
	if(!path_realpathc) {
		path_dname = dirname(path);
		path_realpathc = realpath(path_dname, NULL);
	}
	if(!path_realpathc) {
		log_error("Unable to get real path for '%s'\n", path);
		res = -2;
		goto exit;
	}

	log_debug("Check that path '%s' is under '%s'\n", path_realpathc, base_dirc);
	if(strstr(path_realpathc, base_dirc) == path_realpathc) {
		res = 0;
	} else {
		log_error("Path '%s' is not in base dir '%s'\n", path_realpathc, base_dirc);
	}
exit:
	mem_free(base_realpathc);
	mem_free(base_dirc);
	mem_free(path_realpathc);
	return res;  
}

// open a file for reading, writing, or appending
static int open_file(endpoint_t *ep, int tfd, const char *buf, const char *mode) {
	int er = ERROR_FAULT;
	File *file;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	log_info("open file in dir %s with name %s\n", fsep->curpath, buf);

	char *fullname = malloc_path(fsep->curpath, buf);
	if(path_under_base(fullname, fsep->basepath)) return ERROR_FAULT;

	char *dirc = mem_alloc_str(fullname); char *path     = dirname(dirc);
	char *fnc  = mem_alloc_str(fullname); char *filename = basename(fnc);

	FILE *fp = open_first_match(path, filename, mode);	
	if(fp) {
	
		file = reserve_file(ep, tfd);

		if (file) {
			file->fp = fp;
			file->dp = NULL;
			er = ERROR_OK;
		} else {
			fclose(fp);
			log_error("Could not reserve file\n");
			er = ERROR_FAULT;
		}

	} else {
		
		log_errno("Error opening file '%s/%s'", path, filename);
		er = errno_to_error(errno);
	}

	log_info("OPEN_RD/AP/WR(%s: %s (@ %p))=%p (fp=%p)\n",mode, filename, filename, (void*)file, (void*)fp);

	mem_free(dirc); mem_free(fnc);
	return er;
}

// open a directory read
static int open_dr(endpoint_t *ep, int tfd, const char *buf) {

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

        File *file = reserve_file(ep, tfd);
	
	if (file != NULL) {

		// save pattern for later comparisons
		strcpy(file->dirpattern, buf);
		DIR *dp = opendir(fsep->curpath /*buf+FSP_DATA*/);

		log_info("OPEN_DR(%s)=%p, (chan=%d, file=%p, dp=%p)\n",buf,(void*)dp,
							tfd, (void*)file, (void*)dp);

		if(dp) {
		  file->fp = NULL;
		  file->dp = dp;
		  file->is_first = 1;
		  return ERROR_OK;
		} else {
		  log_errno("Error opening directory");
		  int er = errno_to_error(errno);
		  close_fd(file);
		  return er;
		}
	}
	return ERROR_FAULT;
}

// read directory
//
// Note: there is a race condition, as we do not save the current directory path
// on directory open, so if it is changed "in the middle" of the operation,
// we run into trouble here. Hope noone will do that...
//
// returns the number of bytes read (>= 0), or a negative error number
//
static int read_dir(endpoint_t *ep, int tfd, char *retbuf, int len, int *eof) {

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	File *file = find_file(ep, tfd);

	//log_debug("read_dir: file=%p, is_first=%d\n", file, (file == NULL) ? -1 : file->is_first);

	if (file != NULL) {
		int rv = 0;
		  if (file->is_first) {
		    file->is_first = 0;
		    int l = dir_fill_header(retbuf, 0, file->dirpattern);
		    rv = l;
		    file->de = dir_next(file->dp, file->dirpattern);
		    return rv;
		  }
		  if(!file->de) {
		    close_fds(ep, tfd);
		    *eof = 1;
		    int l = dir_fill_disk(retbuf);
		    rv = l;
		    return rv;
		  }
		  int l = dir_fill_entry(retbuf, fsep->curpath, file->de, len);
		  rv = l;
		  // prepare for next read (so we know if we're done)
		  file->de = dir_next(file->dp, file->dirpattern);
		  return rv;
	}
	return -ERROR_FAULT;
}

// read file data
//
// returns positive number of bytes read, or negative error number
//
static int read_file(endpoint_t *ep, int tfd, char *retbuf, int len, int *eof) {
	File *file = find_file(ep, tfd);

	if (file != NULL) {
		int rv = 0;

		  FILE *fp = file->fp;

		  int n = fread(retbuf, 1, len, fp);
		  rv = n;
		  if(n<len) {
		    // short read, so either error or eof
		    *eof = 1;
		    close_fds(ep, tfd);
		  } else {
		    // as feof() does not let us know if the file is EOF without
		    // having attempted to read it first, we need this kludge
		    int eofc = fgetc(fp);
		    if (eofc < 0) {
		      // EOF
		      *eof = 1;
		      //printf("Setting EOF!\n");
		      close_fds(ep, tfd);
		    } else {
		      // restore fp, so we can read it properly on the next request
		      ungetc(eofc, fp);
		      // do not send EOF
		    }
		  }
		return rv;
	}
	return -ERROR_FAULT;
}

// write file data
static int write_file(endpoint_t *ep, int tfd, char *buf, int len, int is_eof) {
	File *file = find_file(ep, tfd);

	//log_debug("write_file: file=%p\n", file);

	if (file != NULL) {

		FILE *fp = file->fp;
		if(fp) {
		  // TODO: evaluate return value
		  int n = fwrite(buf, 1, len, fp);
		  if (n < len) {
			// short write indicates an error
			log_debug("Short write on file!\n");
			close_fds(ep, tfd);
			return -ERROR_WRITE_ERROR;
		  }
		  if(is_eof) {
		    log_debug("Write file received an EOF\n");
		    close_fds(ep, tfd);
		  }
		  return ERROR_OK;
		}
	}
	return -ERROR_FAULT;
}

// ----------------------------------------------------------------------------------
// command channel

static int _delete_callback(const int num_of_match, const char *name) {

	printf("%d: Calling DELETE on: %s\n", num_of_match, name);

	if (unlink(name) < 0) {
		// error handling
		log_errno("While trying to unlink");

		return -errno_to_error(errno);
	}
	return ERROR_OK;	
}

static int fs_delete(endpoint_t *ep, char *buf, int *outdeleted) {

	int matches = 0;
	char *p = buf;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	do {
		// comma is file pattern separator
		char *pnext = index(p, ',');
		if (pnext != NULL) {
			*pnext = 0;	// write file name terminator (replacing the ',')
		}
		
		int rv = dir_call_matches(fsep->curpath, p, _delete_callback);	
		if (rv < 0) {
			// error happened
			return -rv;
		}
		matches += rv;

		p = (pnext == NULL) ? NULL : pnext+1;
	} 
	while (p != NULL);

	*outdeleted = matches;

	return ERROR_SCRATCHED;	// FILES SCRATCHED message
}	

static int fs_rename(endpoint_t *ep, char *buf) {

	int er = ERROR_FAULT;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	// first find the two names separated by "="
	int p = 0;
	while (buf[p] != 0 && buf[p] != '=') {
		p++;
	}
	if (buf[p] == 0) {
		// not found
		log_error("Did not find '=' in rename command %s\n", buf);
		return ERROR_SYNTAX_NONAME;
	}

	buf[p] = 0;
	char *from = buf+p+1;
	char *to = buf;

	if (index(to, '/') != NULL) {
		// no separator char
		log_error("target file name contained dir separator\n");
		return ERROR_SYNTAX_DIR_SEPARATOR;
	}

	char *frompath = malloc_path(fsep->curpath, from);
	char *topath = malloc_path(fsep->curpath, to);

	char *fromreal = realpath(frompath, NULL);
	mem_free(frompath);
	char *toreal = realpath(topath, NULL);

	if (toreal != NULL) {
		// target already exists
		er = ERROR_FILE_EXISTS;
	} else {
		// check both paths against container boundaries
		if ((strstr(fromreal, fsep->basepath) == fromreal)
			&& (strstr(topath, fsep->basepath) == topath)) {
			// ok

			int rv = rename(fromreal, topath);

			if (rv < 0) {
				er = errno_to_error(errno);
				log_errno("Error renaming a file\n");
			} else {
				er = ERROR_OK;
			}
		}
	}
	mem_free(topath);
	mem_free(toreal);
	mem_free(fromreal);

	return er;
}


static int fs_cd(endpoint_t *ep, char *buf) {
	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	log_debug("Change dir to: %s\n", buf);

	//  concat new path to current path
	char *newpath = malloc_path(fsep->curpath, buf);

	// canonicalize it
	char *newreal = realpath(newpath, NULL);
	// free buffer so we don't forget it
	mem_free(newpath);

	log_debug("Checking that new path '%s' is under base '%s'\n",
		newreal, fsep->basepath);

	if (newreal == NULL) {
		// target does not exist
		return ERROR_FILE_NOT_FOUND;
	}

	// check if the new path is still under the base path
	if (strstr(newreal, fsep->basepath) == newreal) {
		// the needle base path is found at the start of the new real path -> ok
		mem_free(fsep->curpath);
		fsep->curpath = newreal;
	} else {
		// needle (base path) is not in haystack (new path)
		// -> security error
		log_error("Tried to chdir outside base dir %s, to %s\n", fsep->basepath, newreal);
		log_debug("dumb_realpath_root_ptr: %p newreal: %p\n", dumb_realpath_root_ptr, newreal);
		log_debug("NULL: %p\n, strcmp(newreal,\"/\"): %d\n", NULL, strcmp(newreal,"/"));
		mem_free(newreal);
		return ERROR_NO_PERMISSION;
	}
	return ERROR_OK;
}

static int fs_mkdir(endpoint_t *ep, char *buf) {

	int er = ERROR_DIR_ERROR;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	char *newpath = malloc_path(fsep->curpath, buf);

	char *newreal = realpath(newpath, NULL);

	if (newreal != NULL) {
		// file or directory exists
		er = ERROR_FILE_EXISTS;
	} else {
		mode_t oldmask=umask(0);
		int rv = mkdir(newpath, 0755);
		umask(oldmask);

		if (rv < 0) {
			log_errno("Error trying to make a directory");
			er = errno_to_error(errno);
		} else {
			// ok
			er = ERROR_OK;
		}
	}
	mem_free(newpath);
	mem_free(newreal);
	return er;
}


static int fs_rmdir(endpoint_t *ep, char *buf) {

	int er = ERROR_FAULT;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	char *newpath = malloc_path(fsep->curpath, buf);

	char *newreal = realpath(newpath, NULL);
	mem_free(newpath);

	if (newreal == NULL) {
		// directory does not exist
		er = ERROR_FILE_NOT_FOUND;
	} else
	if (strstr(newreal, fsep->basepath) == newreal) {
		// current path is still at the start of new path
		// so it is not broken out of the container
		
		int rv = rmdir(newreal);

		if (rv < 0) {
			er = errno_to_error(errno);
			log_errno("Error trying to remove a directory");
		} else {
			// ok
			er = ERROR_OK;
		}
	}
	mem_free(newreal);
	return er;
}



// ----------------------------------------------------------------------------------

static int open_file_rd(endpoint_t *ep, int tfd, const char *buf) {
       return open_file(ep, tfd, buf, "rb");
}

static int open_file_wr(endpoint_t *ep, int tfd, const char *buf) {
       return open_file(ep, tfd, buf, "wb");
}

static int open_file_ap(endpoint_t *ep, int tfd, const char *buf) {
       return open_file(ep, tfd, buf, "ab");
}


static int readfile(endpoint_t *ep, int chan, char *retbuf, int len, int *eof) {

	File *f = find_file(ep, chan); // ((fs_endpoint_t*)ep)->files;
#ifdef DEBUG_READ
	log_debug("readfile chan %d: file=%p (fp=%p, dp=%p, eof=%d)\n", 
		chan, f, f==NULL ? NULL : f->fp, f == NULL ? NULL : f->dp, *eof);
#endif
	int rv = 0;

	if (f->dp) {
		rv = read_dir(ep, chan, retbuf, len, eof);
	} else
	if (f->fp) {
		// read a file
		rv = read_file(ep, chan, retbuf, len, eof);
	}
	return rv;
}


provider_t fs_provider = {
	"fs",
	fsp_init,
	fsp_new,
	fsp_free,
	close_fds,
	open_file_rd,
	open_file_wr,
	open_file_ap,
	NULL, //open_file_rw,
	open_dr,
	readfile,
	write_file,
	fs_delete,
	fs_rename,
	fs_cd,
	fs_mkdir,
	fs_rmdir
};


