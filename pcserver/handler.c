/****************************************************************************

    Serial line filesystem server
    Copyright (C) 2013 Andre Fachat

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


#include <sys/types.h>
#include <string.h>

#include "registry.h"
#include "handler.h"
#include "errors.h"
#include "log.h"
#include "mem.h"
#include "provider.h"
#include "wireformat.h"
#include "wildcard.h"
#include "openpars.h"



// This file defines the file type handler interface. This is used
// to "wrap" files so that for example x00 (like P00, R00, ...) files 
// or compressed files can be handled.
//
// In the long run the di_provider will become a handler as well - so
// you can CD into a D64 file stored in a D81 image read from an FTP 
// server...

static registry_t handlers;

/*
 * register a new provider, usually called at startup
 */
void handler_register(handler_t *handler) {
	
	reg_append(&handlers, handler);
}

/*
 * initialize the provider registry
 */
void handler_init(void) {

	reg_init(&handlers, "handlers", 10);
}

// unused as of now
file_t* handler_find(file_t *parent, uint8_t type, const char *name, const char *opts, const char **outname) {

	file_t *newfile = NULL;

	for (int i = 0; ; i++) {
		handler_t *handler = reg_get(&handlers, i);
		if (handler == NULL) {
			// not found
			return NULL;
		}
		int err = handler->resolve(parent, &newfile, type, name, opts, outname);
		if (err != CBM_ERROR_OK) {
			log_error("Got %d as error\n", err);
			return NULL;
		}
	}

	return newfile;
}


//----------------------------------------------------------------------------


int handler_wrap(file_t *infile, uint8_t type, const char *name,  
		const char **outname, file_t **outfile) {

	int err = CBM_ERROR_OK;
	*outfile = infile;

	for (int i = 0; ; i++) {
		handler_t *handler = reg_get(&handlers, i);
		if (handler == NULL) {
			// no handler found
			break;
		}
		err = handler->resolve(infile, outfile, type, name, NULL, outname);
		if (err != CBM_ERROR_OK) {
			log_error("Got %d as error from handler %s for %s\n", 
				err, handler->name, name);
		} else {
			// found a handler
			break;
		}
	}
	return err;
}
	

/*
 * open a file/dir, internally
 *
 * This function takes the file name from an OPEN, and the endpoint defined for the 
 * drive as given in the OPEN, and recursively walks the path parts of that file name.
 *
 * Each path part is checked against the list of handlers for a match
 *
 * It returns the directory, as well as the first match in that directory, both as file_t 
 * objects.
 */

static int handler_resolve(endpoint_t *ep, file_t **outdir, file_t **outfile, 
		const char *inname, const char **outpattern, uint8_t type) {

	const char *outname = NULL;
	int err = CBM_ERROR_FAULT;
	file_t *file = NULL;
	file_t *direntry = NULL;


	int inlen = (inname == NULL) ? 0 : strlen(inname);
	char *name = NULL;
	// canonicalize the name
	if (inlen == 0) {
		// no name given - replace with pattern
		name = mem_alloc_str("*");
	} else
	if (inname[inlen-1] == CBM_PATH_SEPARATOR_CHAR) {
		// name ends with a path separator, add pattern
		name = mem_alloc_c(inlen + 2, "search path");
		strcpy(name, inname);
		strcat(name, "*");
	} else {
		// it's either a single pattern, or a path with a pattern
		name = mem_alloc_str(inname);
	}

	// runtime
	const char *namep = name;
	file_t *current_dir = NULL;
	file_t *wrapped_direntry = NULL;

	// handle root dir case
	// initial file struct, corresponds to root directory of endpoint
	//err = ep_create(ep, chan, outfile, inname, &outname);
	if (*namep == '/') {
		current_dir = ep->ptype->root(ep, 1);
		namep++;
	} else {
		current_dir = ep->ptype->root(ep, 0);
	}

	// through name canonicalization, we know namep does now not point to a 0

	// loop as long as we have filename parts left
	while (current_dir != NULL && namep != NULL && *namep != 0) {

		// loop over directory entries
		// note: loops over all directory entries, as we cannot simply match the name
		// due to the x00 pattern matching madness
		// note: may return NULL in direntry and still err== CBM_ERROR_OK, in case
		// we have an empty directory
		file = NULL;
		while ((err = file->handler->direntry(current_dir, &direntry)) == CBM_ERROR_OK) {

			if (direntry == NULL) {
				break;
			}

			// default end of name (if no handler matches)
			outname = strchr(name, CBM_PATH_SEPARATOR_CHAR);	

			// test each dir entry against the different handlers
			// handlers implement checks e.g. for P00 files and wrap them
			// in another file_t level
			// the handler->resolve() method also matches the name, as the
			// P00 files may contain their own "real" name within them

			handler_wrap(direntry, type, namep, &outname, &wrapped_direntry);
		
			// replace original dir entry with wrapped one	
			if (wrapped_direntry != NULL) {
				file = wrapped_direntry;
				// do not check any further dir entries
				break;
			}

			// no handler match found, thus
			// now check if the original filename matches the pattern
			// outname must be set either to point to the trailing '/', or the trailing 0,
			// but not be set to NULL itself
			if (compare_dirpattern(direntry->handler->getname(direntry), namep, &outname)) {
				// matches, so go on with it
				file = direntry;
				// do not check any further dir entries
				break;
			}

			// close the directory entry, we don't need it anymore
			// (handler de-allocates it if necessary)
			direntry->handler->close(direntry, 0);
			direntry = NULL;
		}

		if (err != CBM_ERROR_OK || file == NULL) {
			break;
		}

		// found our directory entry and wrapped it in file
		// now check if we have/need a container wrapper (with e.g. a ZIP file in a P00 file)
		if (*outname == 0) {
			// no more pattern left, so file should be what was required
			// i.e. we have raw access to container files like D64 or ZIP
			// (which is similar to the "$" file on non-load secondary addresses)
			break;
		}

		// outname points to the path separator trailing the matched file name pattern
		// From name canonicalization that is followed by at least a '*', if not further
		// patterns
		namep = outname + 1;
		current_dir = file;

		// check with the container providers (i.e. endpoint providers)
		// whether to wrap it. Here e.g. d64 or zip files are wrapped into 
		// directory file_t handlers 
		wrapped_direntry = provider_wrap(current_dir);
		if (wrapped_direntry != NULL) {
			current_dir = wrapped_direntry;
		}
	}

	// here we have:
	// - current_dir as the current directory
	// - file, the unwrapped first pattern match in that directory, maybe NULL
	// - a possibly large list of opened files, reachable through current_dir->parent etc
	// - namep pointing to the directory match pattern for the current directory

	*outdir = NULL;
	*outfile = NULL;
	if (outpattern != NULL) {
		*outpattern = NULL;
	}
	if (err == CBM_ERROR_OK) {
		*outdir = current_dir;
		*outfile = file;
		if (outpattern != NULL) {
			*outpattern = mem_alloc_str(namep);
		}
	} else {
		// this should close all parents as well
		file->handler->close(file, 1);
	}
	mem_free(name);
	
	return err;
}
	
/*
 * open a file, from fscmd
 *
 * Uses handler_resolve() from above to do the bulk work
 */

int handler_resolve_file(endpoint_t *ep, file_t **outfile, 
		const char *inname, const char *opts, uint8_t type) {

	int err = CBM_ERROR_FAULT;
	file_t *dir = NULL;
	file_t *file = NULL;
	const char *pattern = NULL;
	uint16_t reclen;
	uint8_t filetype;

	openpars_process_options((uint8_t*)opts, &filetype, &reclen);

	err = handler_resolve(ep, &dir, &file, inname, &pattern, type);

	if (err == CBM_ERROR_OK) {
		
		switch (type) {
		case FS_OPEN_RD:
			if (file == NULL) {
				err = CBM_ERROR_FILE_NOT_FOUND;
			} else {
				*outfile = file;
				err = file->handler->seek(file, 0, SEEKFLAG_ABS);
			}
			break;
		case FS_OPEN_WR:
			if (file != NULL) {
				err = CBM_ERROR_FILE_EXISTS;
				break;
			}
			// fall-through
		case FS_OPEN_AP:
		case FS_OPEN_OW:
		case FS_OPEN_RW:
			if (file == NULL) {
				err = dir->handler->create(dir, &file, pattern, filetype, reclen);
				if (err != CBM_ERROR_OK) {
					break;
				}
			}
			if (filetype != FS_DIR_TYPE_UNKNOWN 
				&& file->handler->filetype(file) != filetype) {
				err = CBM_ERROR_FILE_TYPE_MISMATCH;
				break; 
			}
			if (!file->handler->iswriteable(file)) {
				err = CBM_ERROR_WRITE_PROTECT;
				break;
			}
			break;
		}

		if (err == CBM_ERROR_OK) {
			if (reclen != 0) {
				if (file->handler->recordlen(file) != reclen
					|| file->handler->filetype(file) != FS_DIR_TYPE_REL) {
					err = CBM_ERROR_RECORD_NOT_PRESENT;
				}
			}
		}

		if (type == FS_OPEN_AP) {
			err = file->handler->seek(file, 0, SEEKFLAG_END);
		}

		// we want the file here, so we can close the dir and its parents
		dir->handler->close(dir, 1);


		if (err == CBM_ERROR_OK) {
			*outfile = file;
		} else {
			// on error
			if (file != NULL) {
				file->handler->close(file, 0);
			}
			*outfile = NULL;
		}
	}

	if (pattern != NULL) {
		mem_free((char*)pattern);
	}

	return err;
}


/*
 * open a directory, from fscmd
 *
 * Uses handler_resolve() from above to do the bulk work
 */

int handler_resolve_dir(endpoint_t *ep, file_t **outdir, 
		const char *inname, const char *opts) {

	int err = CBM_ERROR_FAULT;
	file_t *dir = NULL;
	file_t *file = NULL;
	const char *pattern = NULL;
	uint16_t reclen;
	uint8_t filetype;

	openpars_process_options((uint8_t*)opts, &filetype, &reclen);

	err = handler_resolve(ep, &dir, &file, inname, &pattern, FS_OPEN_DR);

	// we can close the parents anyway
	file_t *parent = dir->parent;
	if (parent != NULL) {
		parent->handler->close(parent, 1);
	}

	if (err == CBM_ERROR_OK) {
		
		// init directory traversal
		dir->dirstate = DIRSTATE_FIRST;
		dir->pattern = mem_alloc_str(pattern);
		dir->firstmatch = file;		// maybe NULL
	
		*outdir = dir;	
	} else {
		if (dir != NULL) {
			dir->handler->close(dir, 0);
		}
		if (file != NULL) {
			file->handler->close(file, 0);
		}
	}
		
	if (pattern != NULL) {
		mem_free((char*)pattern);
	}

	return err;
}

int handler_direntry(file_t *dir, file_t **direntry) {

	int err = CBM_ERROR_OK;
	file_t *match = NULL;

	// for now, ignore header and blocks free
	if (dir->firstmatch != NULL) {
		match = dir->firstmatch;
		dir->firstmatch = NULL;
		err = CBM_ERROR_OK;
	} else {
		err = dir->handler->direntry(dir, &match);
	}

	if (err == CBM_ERROR_OK) {
		*direntry = match;
	}

	return err;
}

