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

#include "os.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>

#include "dir.h"
#include "fscmd.h"
#include "provider.h"
#include "errors.h"
#include "mem.h"
#include "wireformat.h"
#include "channel.h"
#include "os.h"

#include "log.h"

#undef DEBUG_READ
#define DEBUG_CMD
#define DEBUG_BLOCK

#define	MAX_BUFFER_SIZE	64

//#define	min(a,b)	(((a)<(b))?(a):(b))

typedef struct {
	int		chan;		// channel for which the File is
	FILE		*fp;
	DIR 		*dp;
	char		dirpattern[MAX_BUFFER_SIZE];
	struct dirent	*de;
	unsigned int	is_first :1;	// is first directory entry?
	char		*block;		// direct channel block buffer, 256 byte when allocated
	unsigned char	block_ptr;
} File;

typedef struct {
	// derived from endpoint_t
	endpoint_t	 	base;
	// payload
	char			*basepath;			// malloc'd base path
	char			*curpath;			// malloc'd current path
	File 			files[MAXFILES];
} fs_endpoint_t;

static type_t block_type = {
	"direct_buffer",
	sizeof(char[256])
};

void fsp_init() {
}

extern provider_t fs_provider;

static void init_fp(File *fp) {

	//log_debug("initializing fp=%p (used to be chan %d)\n", fp, fp == NULL ? -1 : fp->chan);

        fp->chan = -1;
	fp->fp = NULL;
	fp->dp = NULL;
	fp->block = NULL;
	fp->block_ptr = 0;
}

// close a file descriptor
static int close_fd(File *file) {

	if (file->chan < 0) {
		return CBM_ERROR_NO_CHANNEL;
	}
	log_debug("Closing file descriptor %p for file %d\n", file, file == NULL ? -1 : file->chan);
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
	if (file->block != NULL) {
		mem_free(file->block);
		file->block = NULL;
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

	if((path == NULL) || (*path == 0)) {
		log_error("Empty path for assign");
		return NULL;
	}

	log_debug("Setting fs endpoint to '%s'\n", path);

	fs_endpoint_t *parentep = (fs_endpoint_t*) parent;

	// alloc and init a new endpoint struct
	fs_endpoint_t *fsep = malloc(sizeof(fs_endpoint_t));
	fsep->basepath = NULL;
	fsep->curpath = NULL;
        for(int i=0;i<MAXFILES;i++) {
		init_fp(&(fsep->files[i]));
        }

	fsep->base.ptype = &fs_provider;

	char *dirpath = malloc_path((parentep == NULL) ? NULL : parentep->curpath,
				path);

	// malloc's a buffer and stores the canonical real path in it
	fsep->basepath = os_realpath(dirpath);

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

static endpoint_t *fsp_temp(char **name) {

	// make path relative
	while (**name == dir_separator_char()) {
		(*name)++;
	}

	// cut off last filename part (either file name or dir mask)
	char *end = strrchr(*name, dir_separator_char());

	fs_endpoint_t *fsep = NULL;

	if (end != NULL) {
		// we have a '/'
		*end = 0;
		fsep = (fs_endpoint_t*) fsp_new(NULL, *name);
		*name = end+1;	// filename part
	} else {
		// no '/', so only mask, path is root
		fsep = (fs_endpoint_t*) fsp_new(NULL, ".");
	}

	// replace computed base path with current working dir to ensure no breakout
	free(fsep->basepath);
	// might get into os.c (Linux (m)allocates the buffer automatically in the right size)
	fsep->basepath = getcwd(NULL, 0);

	return (endpoint_t*) fsep;
}

// ----------------------------------------------------------------------------------
// error translation

static int errno_to_error(int err) {

	switch(err) {
	case EEXIST:
		return CBM_ERROR_FILE_EXISTS;
	case EACCES:
		return CBM_ERROR_NO_PERMISSION;
	case ENAMETOOLONG:
		return CBM_ERROR_FILE_NAME_TOO_LONG;
	case ENOENT:
		return CBM_ERROR_FILE_NOT_FOUND;
	case ENOSPC:
		return CBM_ERROR_DISK_FULL;
	case EROFS:
		return CBM_ERROR_WRITE_PROTECT;
	case ENOTDIR:	// mkdir, rmdir
	case EISDIR:	// open, rename
		return CBM_ERROR_FILE_TYPE_MISMATCH;
	case ENOTEMPTY:
		return CBM_ERROR_DIR_NOT_EMPTY;
	case EMFILE:
		return CBM_ERROR_NO_CHANNEL;
	case EINVAL:
		return CBM_ERROR_SYNTAX_INVAL;
	default:
		return CBM_ERROR_FAULT;
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

			log_debug("reserving file %p for chan %d\n", fp, chan);

                        return &(cep->files[i]);
                }
        }
        log_warn("Did not find free fs session for channel=%d\n", chan);
        return NULL;
}

static File *find_file(endpoint_t *ep, int chan) {
        fs_endpoint_t *cep = (fs_endpoint_t*) ep;

        for (int i = 0; i < MAXFILES; i++) {
                if (cep->files[i].chan == chan) {
                        return &(cep->files[i]);
                }
        }
        log_warn("Did not find fs session for channel=%d\n", chan);
        return NULL;
}

// ----------------------------------------------------------------------------------
// helpers

static char *safe_dirname (const char *path) {
/* a dirname that leaves it's parameter unchanged and doesn't
 * overwrite its result at subsequent calls. Allocates memory
 * that should be free()ed later */
	char *pathc, *dirname_result, *mem_dirname;

	pathc = mem_alloc_str(path);
	dirname_result = dirname(pathc);
	mem_dirname = mem_alloc_str(dirname_result);
	mem_free(pathc);
	return mem_dirname;
}

static char *safe_basename (const char *path) {
/* a basename that leaves it's parameter unchanged and doesn't
 * overwrite it's result at subsequent calls. Allocates memory
 * that should be free()ed later */
	char *pathc, *basename_result, *mem_basename;

	pathc = mem_alloc_str(path);
	basename_result = basename(pathc);
	mem_basename = mem_alloc_str(basename_result);
	mem_free(pathc);
	return mem_basename;
}

static int path_under_base(const char *path, const char *base) {
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

	base_realpathc = os_realpath(base);
	if(!base_realpathc) {
		res = -1;
		log_error("Unable to get real path for '%s'\n", base);
		goto exit;
	}
	base_dirc = mem_alloc_c(strlen(base_realpathc) + 2, "base realpath/");
	if(!base_dirc) {
		res = -3;
		goto exit;
	}
	strcpy(base_dirc, base_realpathc);
	strcat(base_dirc, dir_separator_string());

	path_realpathc = os_realpath(path);
	if(!path_realpathc) {
		path_dname = safe_dirname(path);
		path_realpathc = os_realpath(path_dname);
	}
	if(!path_realpathc) {
		log_error("Unable to get real path for '%s'\n", path);
		res = -2;
		goto exit;
	}
	path_realpathc = realloc(path_realpathc, strlen(path_realpathc) + 2);	// don't forget the null
	if(!path_realpathc) return -3;
	strcat(path_realpathc, dir_separator_string());

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
	mem_free(path_dname);
	return res;
}

// ----------------------------------------------------------------------------------
// commands as sent from the device

// ----------------------------------------------------------------------------------
// block command handling

static int open_block_channel(File *fp) {

	log_debug("Opening block channel %p\n", fp);

	fp->block = mem_alloc(&block_type);
	if (fp->block == NULL) {
		// alloc failed

		log_warn("Buffer memory alloc failed!");

		return CBM_ERROR_NO_CHANNEL;
	} else {
		memset(fp->block, 0, 256);
	}
	fp->block_ptr = 0;

	return CBM_ERROR_OK;
}


// in Firmware currently used for:
// B-A/B-F/U1/U2
static int fs_direct(endpoint_t *ep, char *buf, char *retbuf, int *retlen) {

	// Note that buf has already consumed the drive (first byte), so all indexes are -1
	unsigned char cmd = buf[FS_BLOCK_PAR_CMD-1];
	unsigned int track = (buf[FS_BLOCK_PAR_TRACK-1]&0xff) | ((buf[FS_BLOCK_PAR_TRACK]<<8)&0xff00);
	unsigned int sector = (buf[FS_BLOCK_PAR_SECTOR-1]&0xff) | ((buf[FS_BLOCK_PAR_SECTOR]<<8)&0xff00);
	unsigned char channel = buf[FS_BLOCK_PAR_CHANNEL-1];

	log_debug("DIRECT cmd: %d, tr=%d, se=%d, chan=%d\n", cmd, track, sector, channel);

	File *file = NULL;

	// (bogus) check validity of parameters, otherwise fall through to error
	// need to be validated for other commands besides U1/U2
	if (sector > 0 && sector < 100 && track < 100) {
		switch (cmd) {
		case FS_BLOCK_U1:
			// U1 basically opens a short-lived channel to read the contents of a
			// buffer into the device
			// channel is closed by device with separate FS_CLOSE

			file = reserve_file(ep, channel);
			open_block_channel(file);
			// copy the file contents into the buffer
			// test
			for (int i = 0; i < 256; i++) {
				file->block[i] = i;
			}

			channel_set(channel, ep);
		
			return CBM_ERROR_OK;
		case FS_BLOCK_U2:
			// U2 basically opens a short-lived channel to write the contents of a
			// buffer from the device
			// channel is closed by device with separate FS_CLOSE
			file = reserve_file(ep, channel);
			open_block_channel(file);

			channel_set(channel, ep);
		
			return CBM_ERROR_OK;
		}
	}

	retbuf[0] = track & 0xff;
	retbuf[1] = (track >> 8) & 0xff;
	retbuf[2] = sector & 0xff;
	retbuf[3] = (sector >> 8) & 0xff;
	*retlen = 4;

	return CBM_ERROR_ILLEGAL_T_OR_S;
}

static int read_block(endpoint_t *ep, int tfd, char *retbuf, int len, int *eof) {
	File *file = find_file(ep, tfd);

#ifdef DEBUG_BLOCK
	log_debug("read_block: file=%p, len=%d\n", file, len);
#endif

	if (file != NULL) {
		int avail = 256 - file->block_ptr;
		int n = len;
		if (len >= avail) {
			n = avail;
			*eof = READFLAG_EOF;
		}

#ifdef DEBUG_BLOCK
		log_debug("read_block: avail=%d, n=%d\n", avail, n);
#endif

		if (n > 0) {
			memcpy(retbuf, file->block+file->block_ptr, n);
			file->block_ptr += n;
		}
		return n;
	}
	return -CBM_ERROR_FAULT;
}

static int write_block(endpoint_t *ep, int tfd, char *buf, int len, int is_eof) {
	File *file = find_file(ep, tfd);

	log_debug("write_block: file=%p, len=%d\n", file, len);

	if (file != NULL) {
		int avail = 256 - file->block_ptr;
		int n = len;
		if (len >= avail) {
			n = avail;
		}

		log_debug("read_block: avail=%d, n=%d\n", avail, n);

		if (n > 0) {
			memcpy(file->block+file->block_ptr, buf, n);
			file->block_ptr += n;
		}

#ifdef DEBUG_BLOCK
		log_debug("Block:");
		for (int i = 0; i < 256; i++) {
			log_debug(" %02x", file->block[i] & 0xff);
		}
#endif
		return CBM_ERROR_OK;
	}
	return -CBM_ERROR_FAULT;
}

// ----------------------------------------------------------------------------------
// file command handling


// close a file descriptor
static void close_fds(endpoint_t *ep, int tfd) {
	File *file = find_file(ep, tfd); // ((fs_endpoint_t*)ep)->files;
	if (file != NULL) {
		close_fd(file);
		init_fp(file);
	}
}

// open a file for reading, writing, or appending
static int open_file(endpoint_t *ep, int tfd, const char *buf, int fs_cmd) {
	int er = CBM_ERROR_FAULT;
	File *file;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	log_info("open file for fd=%d in dir %s with name %s\n", tfd, fsep->curpath, buf);

	char *fullname = malloc_path(fsep->curpath, buf);
	os_patch_dir_separator(fullname);
	if(path_under_base(fullname, fsep->basepath)) {
		mem_free(fullname);
		return CBM_ERROR_NO_PERMISSION;
	}

	char *path     = safe_dirname(fullname);
	char *filename = safe_basename(fullname);

	char *options;
	int file_required = FALSE;
	int file_must_not_exist = FALSE;

	switch(fs_cmd) {
		case FS_OPEN_RD:
			options = "rb";
			file_required = TRUE;
			break;
		case FS_OPEN_WR:
			options = "wb";
			file_must_not_exist = TRUE;
			break;
		case FS_OPEN_AP:
			options = "ap";
			file_required = TRUE;
			break;
		case FS_OPEN_OW:
			options = "wb";
			break;
		default:
			log_error("Internal error: open_file with fs_cmd %d\n", fs_cmd);
			goto exit;
	}

	char *name = find_first_match(path, filename, os_path_is_file);
	if(!name) {
		// something with that name exists that isn't a file
		log_error("Unable to open '%s': not a file\n", filename);
		er = CBM_ERROR_FILE_TYPE_MISMATCH;
		goto exit;
	}
	int file_exists = !access(name, F_OK);
	if(file_required && !file_exists) {
		log_error("Unable to open '%s': file not found\n", name);
		er = CBM_ERROR_FILE_NOT_FOUND;
		goto exit;
	}
	if(file_must_not_exist && file_exists) {
		log_error("Unable to open '%s': file exists\n", name);
		er = CBM_ERROR_FILE_EXISTS;
		goto exit;
	}

	FILE *fp = fopen(name, options);
	if(fp) {

		file = reserve_file(ep, tfd);

		if (file) {
			file->fp = fp;
			file->dp = NULL;
			er = CBM_ERROR_OK;
		} else {
			fclose(fp);
			log_error("Could not reserve file\n");
			er = CBM_ERROR_FAULT;
		}

	} else {

		log_errno("Error opening file '%s/%s'", path, filename);
		er = errno_to_error(errno);
	}

	log_debug("OPEN_RD/AP/WR(%s: %s (@ %p))=%p (fp=%p)\n", options, filename, filename, (void*)file, (void*)fp);

exit:
	mem_free(name); 
	mem_free(path); 
	mem_free(filename);
	return er;
}

// open a directory read
static int open_dr(endpoint_t *ep, int tfd, const char *buf, const char *opts) {

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

       	char *fullname = malloc_path(fsep->curpath, buf);
	os_patch_dir_separator(fullname);
	if(path_under_base(fullname, fsep->basepath)) {
		mem_free(fullname);
		return CBM_ERROR_NO_PERMISSION;
	}
 
	File *file = reserve_file(ep, tfd);

	if (file != NULL) {

		// save pattern for later comparisons
		strcpy(file->dirpattern, buf);
		DIR *dp = opendir(fsep->curpath /*buf+FSP_DATA*/);

		log_debug("OPEN_DR(%s)=%p, (chan=%d, file=%p, dp=%p)\n",buf,(void*)dp,
							tfd, (void*)file, (void*)dp);

		if(dp) {
		  file->fp = NULL;
		  file->dp = dp;
		  file->is_first = 1;
		  return CBM_ERROR_OK;
		} else {
		  log_errno("Error opening directory");
		  int er = errno_to_error(errno);
		  close_fd(file);
		  return er;
		}
	}
	return CBM_ERROR_FAULT;
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
		*eof = READFLAG_DENTRY;
		  if (file->is_first) {
		    file->is_first = 0;
		    int l = dir_fill_header(retbuf, 0, file->dirpattern);
		    rv = l;
		    file->de = dir_next(file->dp, file->dirpattern);
		    return rv;
		  }
		  if(!file->de) {
		    *eof |= READFLAG_EOF;
		    int l = dir_fill_disk(retbuf, fsep->curpath);
		    rv = l;
		    return rv;
		  }
		  int l = dir_fill_entry(retbuf, fsep->curpath, file->de, len);
		  rv = l;
		  // prepare for next read (so we know if we're done)
		  file->de = dir_next(file->dp, file->dirpattern);
		  return rv;
	}
	return -CBM_ERROR_FAULT;
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
		    *eof = READFLAG_EOF;
		    log_debug("short read on %d\n", tfd);
		  } else {
		    // as feof() does not let us know if the file is EOF without
		    // having attempted to read it first, we need this kludge
		    int eofc = fgetc(fp);
		    if (eofc < 0) {
		      // EOF
		      *eof = READFLAG_EOF;
		      log_debug("EOF on read %d\n", tfd);
		    } else {
		      // restore fp, so we can read it properly on the next request
		      ungetc(eofc, fp);
		      // do not send EOF
		    }
		  }
		return rv;
	}
	return -CBM_ERROR_FAULT;
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
			log_debug("Close fd=%d on short write!\n", tfd);
			close_fds(ep, tfd);
			return -CBM_ERROR_WRITE_ERROR;
		  }
		  if(is_eof) {
		    log_debug("Close fd=%d normally on write file received an EOF\n", tfd);
		    close_fds(ep, tfd);
		  }
		  return CBM_ERROR_OK;
		}
	}
	return -CBM_ERROR_FAULT;
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
	return CBM_ERROR_OK;
}

static int fs_delete(endpoint_t *ep, char *buf, int *outdeleted) {

	int matches = 0;
	char *p = buf;

	os_patch_dir_separator(buf);

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	do {
		// comma is file pattern separator
		char *pnext = strchr(p, ',');
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

	return CBM_ERROR_SCRATCHED;	// FILES SCRATCHED message
}

static int fs_rename(endpoint_t *ep, char *nameto, char *namefrom) {

	int er = CBM_ERROR_FAULT;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

#ifdef DEBUG_CMD
	log_debug("fs_rename: '%s' -> '%s'\n", namefrom, nameto);
#endif

	if ((strchr(nameto, '/') != NULL) || (strchr(nameto,'\\') != NULL)) {
		// no separator char
		log_error("target file name contained dir separator\n");
		return CBM_ERROR_SYNTAX_DIR_SEPARATOR;
	}

	char *frompath = malloc_path(fsep->curpath, namefrom);
	char *topath = malloc_path(fsep->curpath, nameto);

	char *fromreal = os_realpath(frompath);
	mem_free(frompath);
	char *toreal = os_realpath(topath);

	if (toreal != NULL) {
		// target already exists
		er = CBM_ERROR_FILE_EXISTS;
	} else
	if (fromreal == NULL) {
		er = CBM_ERROR_FILE_NOT_FOUND;
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
				er = CBM_ERROR_OK;
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

	os_patch_dir_separator(buf);

	log_debug("Change dir to: %s\n", buf);

	//  concat new path to current path
	char *newpath = malloc_path(fsep->curpath, buf);

	// canonicalize it
	char *newreal = os_realpath(newpath);
	if (newreal == NULL) {
		// target does not exist
		log_error("Unable to change dir to '%s'\n", newpath);
		return CBM_ERROR_FILE_NOT_FOUND;
	}

	// free buffer so we don't forget it
	mem_free(newpath);

	// check if the new path is still under the base path
	if(path_under_base(newreal, fsep->basepath)) {
		// -> security error
		mem_free(newreal);
		return CBM_ERROR_NO_PERMISSION;
	}

	// check if the new path really is a directory
	struct stat path;
	if(stat(newreal, &path) < 0) {
		log_error("Could not stat '%s'\n", newreal);
		return CBM_ERROR_DIR_ERROR;
	}
	if(!S_ISDIR(path.st_mode)) {
		log_error("CHDIR: '%s' is not a directory\n", newreal);
		return CBM_ERROR_DIR_ERROR;
	}

	mem_free(fsep->curpath);
	fsep->curpath = newreal;
	return CBM_ERROR_OK;
}

static int fs_mkdir(endpoint_t *ep, char *buf) {

	int er = CBM_ERROR_DIR_ERROR;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	os_patch_dir_separator(buf);

	char *newpath = malloc_path(fsep->curpath, buf);

	char *newreal = os_realpath(newpath);

	if (newreal != NULL) {
		// file or directory exists
		er = CBM_ERROR_FILE_EXISTS;
	} else {
		mode_t oldmask=umask(0);
		int rv = os_mkdir(newpath, 0755);
		umask(oldmask);

		if (rv < 0) {
			log_errno("Error trying to make a directory");
			er = errno_to_error(errno);
		} else {
			// ok
			er = CBM_ERROR_OK;
		}
	}
	mem_free(newpath);
	mem_free(newreal);
	return er;
}


static int fs_rmdir(endpoint_t *ep, char *buf) {

	int er = CBM_ERROR_FAULT;

	fs_endpoint_t *fsep = (fs_endpoint_t*) ep;

	os_patch_dir_separator(buf);

	char *newpath = malloc_path(fsep->curpath, buf);

	char *newreal = os_realpath(newpath);
	mem_free(newpath);

	if (newreal == NULL) {
		// directory does not exist
		er = CBM_ERROR_FILE_NOT_FOUND;
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
			er = CBM_ERROR_OK;
		}
	}
	mem_free(newreal);
	return er;
}



// ----------------------------------------------------------------------------------

static int open_file_rd(endpoint_t *ep, int tfd, const char *buf, const char *opts) {
       return open_file(ep, tfd, buf, FS_OPEN_RD);
}

static int open_file_wr(endpoint_t *ep, int tfd, const char *buf, const char *opts, const int is_overwrite) {
	if (is_overwrite) {
       		return open_file(ep, tfd, buf, FS_OPEN_OW);
	} else {
       		return open_file(ep, tfd, buf, FS_OPEN_WR);
	}
}

static int open_file_ap(endpoint_t *ep, int tfd, const char *buf, const char *opts) {
       return open_file(ep, tfd, buf, FS_OPEN_AP);
}

static int open_file_rw(endpoint_t *ep, int tfd, const char *buf, const char *opts) {
	if (*buf == '#') {
		// ok, open a direct block channel

		File *file = reserve_file(ep, tfd);

		int er = open_block_channel(file);

		if (er) {
			// error
			close_fd(file);
			log_error("Could not reserve file\n");
		}
		return er;

	} else {
		// no support for r/w for "standard" files for now
		return CBM_ERROR_DRIVE_NOT_READY;
	}
}


static int readfile(endpoint_t *ep, int chan, char *retbuf, int len, int *eof) {

	File *f = find_file(ep, chan); // ((fs_endpoint_t*)ep)->files;
#ifdef DEBUG_READ
	log_debug("readfile chan %d: file=%p (fp=%p, dp=%p, block=%p, eof=%d)\n",
		chan, f, f==NULL ? NULL : f->fp, f == NULL ? NULL : f->dp, f == NULL ? NULL : f->block, *eof);
#endif
	int rv = 0;

	if (f->dp) {
		rv = read_dir(ep, chan, retbuf, len, eof);
	} else
	if (f->fp) {
		// read a file
		// standard file read
		rv = read_file(ep, chan, retbuf, len, eof);
	} else
	if (f->block != NULL) {
		// direct channel block buffer read
		rv = read_block(ep, chan, retbuf, len, eof);
	}
	return rv;
}

// write file data
static int writefile(endpoint_t *ep, int chan, char *buf, int len, int is_eof) {
	File *file = find_file(ep, chan);

	int rv = -CBM_ERROR_FAULT;

	//log_debug("write_file: file=%p\n", file);

	if (file != NULL) {

		if (file->block != NULL) {
			rv = write_block(ep, chan, buf, len, is_eof);
		} else {
			rv = write_file(ep, chan, buf, len, is_eof);
		}	
	}
	return rv;
}

// ----------------------------------------------------------------------------------

provider_t fs_provider = {
	"fs",
	"ASCII",
	fsp_init,
	fsp_new,
	fsp_temp,
	fsp_free,
	close_fds,
	open_file_rd,
	open_file_wr,
	open_file_ap,
	open_file_rw,
	open_dr,
	readfile,
	writefile,
	fs_delete,
	fs_rename,
	fs_cd,
	fs_mkdir,
	fs_rmdir,
	NULL,
	fs_direct
};


