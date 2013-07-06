/****************************************************************************

    Serial line filesystem server
    Copyright (C) 2012 Andre Fachat
    Copyright (C) 2012 Nils Eilers

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

/* TODO:
 * - directory listing aborts sometimes. Why?
 * - fix directory stuff for multiple assigns
 * - skip or skip not hidden files
 * - allow wildcards for OPEN ("LOAD *")
 * - allow wildcards for CD/RM etc.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "version.h"
#include "packet.h"
#include "provider.h"
#include "wireformat.h"
#include "petscii.h"
#include "main.h"
#include "debug.h"
#include "led.h"
#include "fatfs/integer.h"
#include "fatfs/ff.h"
#include "sdcard.h"
#include "petscii.h"
#include "fat_provider.h"
#include "dir.h"
#include "dirconverter.h"
#include "charconvert.h"


#define  DEBUG_FAT

/* ----- Glue to firmware -------------------------------------------------------------------- */

static uint8_t current_charset;

static void *prov_assign(const char *name);
static void prov_free(void *epdata);
static void fat_submit(void *epdata, packet_t *buf);
static void fat_submit_call(void *epdata, int8_t channelno, packet_t *txbuf, packet_t *rxbuf,
                uint8_t (*callback)(int8_t channelno, int8_t errnum, packet_t *packet));

static charset_t charset(void *epdata) {
	return current_charset;
}

static void set_charset(void *epdata, charset_t new_charset) {
	current_charset = new_charset;
}

provider_t fat_provider  = {
	prov_assign,
	prov_free,
	charset,
	set_charset,
	fat_submit,
	fat_submit_call,
	directory_converter,
};


/* ----- Private provider data --------------------------------------------------------------- */

static FATFS Fatfs[_VOLUMES];	/* File system object for each logical drive */

#define AVAILABLE -1
enum enum_dir_state { DIR_INACTIVE, DIR_HEAD, DIR_FILES, DIR_FOOTER };
struct {
	int8_t chan;		// entry used by channel # or AVAILABLE
	int8_t dir_state;	// DIR_INACTIVE for files or DIR_* when reading directories
	FIL f;			// file data
} tbl[FAT_MAX_FILES];

#ifndef FAT_MAX_ASSIGNS
#	define FAT_MAX_ASSIGNS 2	/* Each drive has a current directory */
#endif
uint8_t no_of_assigns = 0;		/* Number of assigns/drives */

static DIR dir;
static uint8_t dir_drive;
static char dir_mask[_MAX_LFN+1];
static char dir_headline[_MAX_LFN+1];
static FILINFO Finfo;

/* ----- Prototypes -------------------------------------------------------------------------- */

// helper functions
static int8_t fs_read_dir(void *epdata, int8_t channelno, packet_t *packet);
static int8_t fs_move(char *buf);
static void fs_delete(char *path, packet_t *p);

// debug functions
static void dump_packet(packet_t *p);

/* ----- File / Channel table ---------------------------------------------------------------- */

static void tbl_init(void) {
	for(uint8_t i=0; i < FAT_MAX_FILES; i++) {
		tbl[i].chan = AVAILABLE;
		tbl[i].dir_state = DIR_INACTIVE;
	}
}

static int8_t tbl_chpos(uint8_t chan) {
	for(uint8_t i=0; i < FAT_MAX_FILES; i++) if(tbl[i].chan == chan) return i;
	return -1;
}

static int8_t get_dir_state(uint8_t chan) {
	int8_t pos = tbl_chpos(chan);
	if(pos < 0 ) return pos;
	return tbl[pos].dir_state;
}

static FIL *tbl_ins_file(uint8_t chan) {
	for(uint8_t i=0; i < FAT_MAX_FILES; i++) {
		if(tbl[i].chan == chan) {
			debug_printf("#%d already exists @%d", chan, i); debug_putcrlf();
			tbl[i].dir_state = DIR_INACTIVE;
			return &tbl[i].f;
		}
		if(tbl[i].chan == AVAILABLE) {
			tbl[i].chan = chan;
			tbl[i].dir_state = DIR_INACTIVE;
			debug_printf("#%d found in file table @ %d", chan, i); debug_putcrlf();
			return &tbl[i].f;
		}
	}
	debug_printf("tbl_ins_file: out of memory", chan); debug_putcrlf();
	return NULL;
}

static int8_t tbl_ins_dir(int8_t chan) {
	for(uint8_t i=0; i < FAT_MAX_FILES; i++) {
		if(tbl[i].chan == chan) {
			debug_printf("dir_state #%d := DIR_HEAD", i); debug_putcrlf();
			tbl[i].dir_state = DIR_HEAD;
			return 0;
		}
		if(tbl[i].chan == AVAILABLE) {
			debug_printf("@%d initialized with DIR_HEAD", i); debug_putcrlf();
			tbl[i].chan = chan;
			tbl[i].dir_state = DIR_HEAD;
			return 0;
		}
	}
	return 1;
}
static FIL *tbl_find_file(uint8_t chan) {
	uint8_t pos;

	if((pos = tbl_chpos(chan)) < 0) {
		debug_printf("tbl_find_file: #%d not found!", chan); debug_putcrlf();
		return NULL;
	}
	debug_printf("#%d found @%d", chan, pos); debug_putcrlf();
	return &tbl[pos].f;
}

static FRESULT tbl_close_file(uint8_t chan) {
	uint8_t pos;
	FRESULT res = CBM_ERROR_OK;

	if((pos = tbl_chpos(chan)) != AVAILABLE) {
		FRESULT res = f_close(&tbl[pos].f);
		debug_printf("f_close (#%d @%d): %d", chan, pos, res); debug_putcrlf();
		tbl[pos].chan = AVAILABLE;
	} else {
		debug_printf("f_close (#%d): nothing to do\n", chan);
	}
	return res;
}

/* ----- Provider routines ------------------------------------------------------------------- */

static void *prov_assign(const char *name) {
	/* name = FAT:<parameter> */

	debug_printf("fat_prov_assign name=%s\n", name);
	/* mount (but don't remount) volume, this will always succeed regardless of the drive status */
	if(disk_status(0) & STA_NOINIT) f_mount(0, &Fatfs[0]);

	tbl_init();			// TODO: move this to fat_provider_init();

	return NULL;
}

static void prov_free(void *epdata) {
	// free the ASSIGN-related data structure
	// dummy
}

static void fat_submit(void *epdata, packet_t *buf) {
	// submit a fire-and-forget packet (log_*, terminal)
	// not applicable for storage ==> dummy
}

static void zero_terminate(char *dest, const char *source, uint8_t len) {
	strncpy(dest, source, len);
	dest[len] = 0;
}

static void fat_submit_call(void *epdata, int8_t channelno, packet_t *txbuf, packet_t *rxbuf,
	uint8_t (*callback)(int8_t channelno, int8_t errnum, packet_t *packet))
{
	// submit a request/response packet; call the callback function when the
	// response is received; If callback returns != 0 then the call is kept open,
	// and further responses can be received

	int8_t res = CBM_ERROR_FAULT;
	UINT transferred = 0;
	FIL *fp;
	int8_t ds;
	char *path = (char *) (txbuf->buffer + 1);
	uint8_t len = txbuf->len - 1;

#	ifdef _USE_LFN
		Finfo.lfname = Lfname;
		Finfo.lfsize = sizeof Lfname;
		char *open_name = Lfname;	// zero-terminated filename for FS_OPEN
#	else
		char open_name[17];
#	endif

	switch(txbuf->type) {
		case FS_CHDIR:
			debug_printf("CHDIR into '%s'\n", path);
			res = f_chdir(path);
			packet_write_char(rxbuf, res);
			break;

		case FS_MKDIR:
			debug_printf("MKDIR '%s'\n", path);
			res = f_mkdir(path);
			packet_write_char(rxbuf, res);
			break;

		case FS_RMDIR:
			// will unlink files as well. Should I test first, if "path" is really a directory?
			debug_printf("RMDIR '%s'\n", path);
			res = f_unlink(path);
			packet_write_char(rxbuf, res);
			break;

		case FS_OPEN_RD:
			/* open file for reading (only) */
			fp = tbl_ins_file(channelno);
			if(fp) {
				zero_terminate(open_name, path, len);
				res = f_open(fp, open_name, FA_READ | FA_OPEN_EXISTING);
				debug_printf("FS_OPEN_RD '%s' #%d, res=%d\n", open_name, channelno, res);
				packet_write_char(rxbuf, res);
			} else {
				// too many files!
				packet_write_char(rxbuf, CBM_ERROR_NO_CHANNEL);
			}
			break;

		case FS_OPEN_WR:
			/* open file for writing (only); error if file exists */
			fp = tbl_ins_file(channelno);
			if(fp) {
				zero_terminate(open_name, path, len);
				res = f_open(fp, open_name, FA_WRITE | FA_CREATE_NEW);
				debug_printf("FS_OPEN_WR '%s' #%d, res=%d\n", open_name, channelno, res);
				packet_write_char(rxbuf, res);
			} else {
				// too many files!
				packet_write_char(rxbuf, CBM_ERROR_NO_CHANNEL);
			}
			break;

		case FS_OPEN_RW:
			/* open file for read/write access */
			fp = tbl_ins_file(channelno);
			if(fp) {
				zero_terminate(open_name, path, len);
				res = f_open(fp, open_name, FA_READ | FA_WRITE);
				debug_printf("FS_OPEN_RW '%s' #%d, res=%d\n", open_name, channelno, res);
				packet_write_char(rxbuf, res);
			} else {
				// too many files!
				packet_write_char(rxbuf, CBM_ERROR_NO_CHANNEL);
			}
			break;

		case FS_OPEN_OW:
			/* open file for write only, overwriting. If the file exists it is truncated
			 * and writing starts at the beginning. If it does not exist, create the file */
			fp = tbl_ins_file(channelno);
			if(fp) {
				zero_terminate(open_name, path, len);
				res = f_open(fp, open_name, FA_WRITE | FA_CREATE_ALWAYS);
				debug_printf("FS_OPEN_OW '%s' #%d, res=%d\n", open_name, channelno, res);
				packet_write_char(rxbuf, res);
			} else {
				// too many files!
				packet_write_char(rxbuf, CBM_ERROR_NO_CHANNEL);
			}
			break;

		case FS_OPEN_AP:
			/* open an existing file for appending data to it. Returns an error if it
			 * does not exist */
			fp = tbl_ins_file(channelno);
			if(fp) {
				zero_terminate(open_name, path, len);
				res = f_open(fp, open_name, FA_WRITE | FA_OPEN_EXISTING);
				debug_printf("FS_OPEN_AP '%s' #%d, res=%d\n", open_name, channelno, res);
				/* move to end of file to append data */
				res = f_lseek(fp, f_size(fp));
				debug_printf("Move to EOF to append data: %d\n", res);
				packet_write_char(rxbuf, res);
			} else {
				// too many files!
				packet_write_char(rxbuf, CBM_ERROR_NO_CHANNEL);
			}
			break;

		case FS_OPEN_DR:
			/* open a directory for reading */
			debug_printf("FS_OPEN_DIR for drive %d, ", txbuf->buffer[0]);
			char *b, *d;
			if (txbuf->len > 1) {
				debug_printf("dirmask '%s'\n", txbuf->buffer + 1);
				// If path is a directory, list its contents
				if(f_stat(path, &Finfo) == FR_OK) {
					if(Finfo.fattrib & AM_DIR) {
						debug_printf("'%s' is a directory\n", path);
						res = f_opendir(&dir, path);
						b = splitpath(path, &d);
						strcpy(dir_headline, b);
						strcpy(dir_mask, "*");
					}
				} else {
					b = splitpath(path, &d);
					strcpy(dir_headline, b);
					debug_printf("DIR: %s NAME: %s\n", d, b);
					res = f_opendir(&dir, d);
					strncpy(dir_mask, b, _MAX_LFN);
					dir_mask[_MAX_LFN] = 0;
				}
			} else {
				debug_puts("no dirmask\n");
				strcpy(dir_mask, "*");
				f_getcwd(dir_headline, sizeof(dir_headline));
				// Remove drive string "0:"
				memmove(dir_headline, dir_headline + 2, sizeof(dir_headline) - 2);
				res = f_opendir(&dir, ".");
			}

			dir_drive = txbuf->buffer[0];
			if(tbl_ins_dir(channelno)) {
				res = CBM_ERROR_NO_CHANNEL;
				debug_puts("No channel for FS_OPEN_DR"); debug_putcrlf();
			}
			debug_printf("f_opendir: %d", res); debug_putcrlf();
			packet_write_char(rxbuf, res);
			break;

		case FS_CLOSE:
			/* close a file, ignored when not opened first */
			debug_printf("FS_CLOSE #%d", channelno); debug_putcrlf();
			packet_write_char(rxbuf, tbl_close_file(channelno));
			break;

		case FS_MOVE:
			/* rename / move a file */
			packet_write_char(rxbuf, fs_move(path));
			break;

		case FS_DELETE:
			fs_delete(path, rxbuf);
			break;

		case FS_READ:
			ds = get_dir_state(channelno);
			if(ds < 0 ) {
				debug_printf("No channel found for FS_READ #%d", channelno); debug_putcrlf();
				res = CBM_ERROR_NO_CHANNEL;
			} else if(ds) {
				// Read directory
				res = fs_read_dir(epdata, channelno, rxbuf);
			} else {
				// Read file
				fp = tbl_find_file(channelno);
				res = f_read(fp, rxbuf->buffer, rxbuf->len, &transferred);
				debug_printf("%d/%d bytes read from #%d, res=%d\n", transferred, rxbuf->len, channelno, res);
				rxbuf->wp = transferred;
				if(fp->fptr == fp->fsize) {
					rxbuf->type = FS_EOF;
				} else {
					rxbuf->type = FS_WRITE;
				}

				if(res) rxbuf->type = FS_EOF;
				// TODO: add FS_REPLY when allowed (not yet)
			}
			break;

		case FS_WRITE:
		case FS_EOF:
			fp = tbl_find_file(channelno);
			if(fp) {
				if(txbuf->rp < txbuf->wp) {
					len = txbuf->wp - txbuf->rp;
					res = f_write(fp, txbuf->buffer, len, &transferred);
					debug_printf("%d/%d bytes written to #%d, res=%d\n", transferred, len, channelno, res);
				}
			} else {
				debug_printf("No channel found for FS_WRITE/FS_EOF #%d", channelno); debug_putcrlf();
			}

			break;

		case FS_FORMAT:
		case FS_CHKDSK:
			debug_printf("Command %d unsupported", txbuf->type);
			packet_write_char(rxbuf, CBM_ERROR_SYNTAX_INVAL);
			break;

		default:
			debug_puts("### UNKNOWN CMD ###"); debug_putcrlf();
			dump_packet(txbuf);
			break;
	}
	callback(channelno, res, rxbuf);
}


/* ----- Directories ------------------------------------------------------------------------- */

int8_t fs_read_dir(void *epdata, int8_t channelno, packet_t *packet) {
	int8_t res;
	int8_t tblpos = tbl_chpos(channelno);
	char *p = (char *) packet->buffer;

	switch(get_dir_state(channelno)) {
		case -1:
			/* no channel */
			debug_puts("fs_read_dir: no channel!"); debug_putcrlf();
			packet->type = FS_EOF;
			return -CBM_ERROR_NO_CHANNEL;
			break;

		case DIR_HEAD:
			/* Disk name */
			debug_puts("fs_read_dir/DIR_HEAD"); debug_putcrlf();
			p[FS_DIR_LEN+0] = dir_drive;
			p[FS_DIR_LEN+1] = 0;
			p[FS_DIR_LEN+2] = 0;
			p[FS_DIR_LEN+3] = 0;
			// TODO: add date
			p[FS_DIR_MODE]  = FS_DIR_MOD_NAM;
			strncpy(p+FS_DIR_NAME, dir_headline, 16);
			p[FS_DIR_NAME + 16] = 0;

			tbl[tblpos].dir_state = DIR_FILES;
			packet_update_wp(packet, FS_DIR_NAME + strlen(p+FS_DIR_NAME));
			return 0;

		case DIR_FILES:
			/* Files and directories */
			debug_puts("fs_read_dir/DIR_FILES"); debug_putcrlf();
			for(;;) {
				res = f_readdir(&dir, &Finfo);
				if(res != FR_OK || !Finfo.fname[0]) {
					tbl[tblpos].dir_state = DIR_FOOTER;
					return 0;
				}
				char *filename;
				filename = Finfo.fname;
#				if _USE_LFN
					if(Lfname[0]) filename = Lfname;
#				endif
				if(compare_pattern(filename, dir_mask)) break;
			}

			// TODO: skip or skip not hidden files
			debug_printf("%c%c%c%c%c %u/%02u/%02u %02u:%02u %9lu  '%s'",
					(Finfo.fattrib & AM_DIR) ? 'D' : '-',
					(Finfo.fattrib & AM_RDO) ? 'R' : '-',
					(Finfo.fattrib & AM_HID) ? 'H' : '-',
					(Finfo.fattrib & AM_SYS) ? 'S' : '-',
					(Finfo.fattrib & AM_ARC) ? 'A' : '-',
					(Finfo.fdate >> 9) + 1980, (Finfo.fdate >> 5) & 15, Finfo.fdate & 31,
					(Finfo.ftime >> 11), (Finfo.ftime >> 5) & 63,
					Finfo.fsize, &(Finfo.fname[0]));
#			if _USE_LFN
				for (uint8_t i = strlen(Finfo.fname); i < 14; i++) debug_putc(' ');
				debug_printf("'%s'", Lfname);
#			endif
			debug_putcrlf();

			p[FS_DIR_LEN] = Finfo.fsize & 255;
			p[FS_DIR_LEN+1] = (Finfo.fsize >> 8) & 255;
			p[FS_DIR_LEN+2] = (Finfo.fsize >> 16) & 255;
			p[FS_DIR_LEN+3] = (Finfo.fsize >> 24) & 255;

			p[FS_DIR_YEAR] = (Finfo.fdate >> 9) + 80;
			p[FS_DIR_MONTH] = (Finfo.fdate >> 5) & 15;
			p[FS_DIR_DAY] = Finfo.fdate & 31;
			p[FS_DIR_HOUR] = Finfo.ftime >> 11;
			p[FS_DIR_MIN] = (Finfo.ftime >> 5) & 63;

			p[FS_DIR_MODE] = Finfo.fattrib & AM_DIR ? FS_DIR_MOD_DIR : FS_DIR_MOD_FIL;

#			ifdef _USE_LFN
				if((strlen(Lfname) > 16 ) || (!Lfname[0])) {
					// no LFN or too long LFN ==> use short name
					strcpy(p + FS_DIR_NAME, Finfo.fname);
				} else strcpy(p + FS_DIR_NAME, Lfname);
#			else
				strcpy(p + FS_DIR_NAME, Finfo.fname);
#			endif

			packet_update_wp(packet, FS_DIR_NAME + strlen(p+FS_DIR_NAME));
			return 0;

		case DIR_FOOTER:
		default:
			/* number of free bytes / end of directory */
			debug_puts("fs_read_dir/DIR_FOOTER"); debug_putcrlf();
			FATFS *fs = &Fatfs[0];
			DWORD free_clusters;
			DWORD free_bytes = 0;	/* fallback default size */
			int8_t res = f_getfree("0:/", &free_clusters, &fs);
			if(res == FR_OK) {
				// assuming 512 bytes/sector ==> * 512 ==> << 9
				free_bytes = (free_clusters * fs->csize) << 9;
			} else
			{
				debug_printf("f_getfree: %d\n", res);
			}
			p[FS_DIR_LEN] = free_bytes & 255;
			p[FS_DIR_LEN+1] = (free_bytes >> 8) & 255;
			p[FS_DIR_LEN+2] = (free_bytes >> 16) & 255;
			p[FS_DIR_LEN+3] = (free_bytes >> 24) & 255;
			p[FS_DIR_MODE]  = FS_DIR_MOD_FRE;
			p[FS_DIR_NAME] = 0;
			tbl[tblpos].dir_state = DIR_INACTIVE;
			tbl[tblpos].chan = AVAILABLE;
			packet_update_wp(packet, FS_DIR_NAME + strlen(p+FS_DIR_NAME));
			packet->type = FS_EOF;
			return 0;
	}
}

/* ----- Rename a file or directory ---------------------------------------------------------- */

static int8_t fs_move(char *buf) {
	/* Rename/move a file or directory
	 * DO NOT RENAME/MOVE OPEN OBJECTS!
	 */
	int8_t er = CBM_ERROR_FAULT;
	uint8_t p = 0;
	char *from, *to;
	FILINFO fileinfo;

	// first find the two names separated by "="
	while (buf[p] != 0 && buf[p] != '=') p++;
	if (!buf[p]) return CBM_ERROR_SYNTAX_NONAME;

	buf[p] = 0;
	from = buf + p + 1;
	to = buf;

	debug_printf("FS_MOVE '%s' to '%s'", from, to); debug_putcrlf();

	if((er = f_stat(to, &fileinfo)) == CBM_ERROR_OK) return CBM_ERROR_FILE_EXISTS;
	if(er != FR_NO_FILE) return er;

	return f_rename(from, to);
}


/* ----- Delete files ------------------------------------------------------------------------ */

int8_t _scratch(const char *path) {
	debug_printf("Scratching '%s'\n", path);
	int8_t res = f_unlink(path);
	if(res) debug_printf("f_unlink: %d\n", res);
	return res;
}

/* Deletes one or more file masks separated by commas
 * Limits the reported number of scratched files to 99
 * Returns CBM_ERROR_SCRATCHED plus number of scratched files
 * Returns only the error but not the number of scratched files in case of any errors
 */
static void fs_delete(char *path, packet_t *packet) {
	int8_t res;
	uint16_t files_scratched = 0;
	char *pnext;

	while(path) {
		pnext = strchr(path, ',');
		if(pnext) *pnext = 0;
		debug_printf("Scratching '%s'...\n", path);

		res = traverse(path, 
			0, 			// don't limit number of files to scratch
			&files_scratched, 	// counts matches
			0, 			// no special file attributes required
			AM_DIR | AM_RDO,	// ignore directories and read-only-files
			_scratch);

		path = pnext ? pnext + 1 : NULL;
	}

	packet_write_char(packet, CBM_ERROR_SCRATCHED);
	packet_write_char(packet, (files_scratched > 99) ? 99 : files_scratched);

}

/* ----- Debug routines ---------------------------------------------------------------------- */

static void dump_packet(packet_t *p)
{
	uint16_t tot = 0;
	uint8_t line = 0;
	uint8_t x = 0;

	debug_puts("--- dump packet ---"); debug_putcrlf();
	debug_printf("ptr: %p ", p);
	debug_printf("type: %d ", p->type);
	debug_printf("chan: %d   ", p->chan);
	debug_printf("rp: %d wp: %d len: %d ", p->rp, p->wp, p->len);
	debug_putcrlf();
	if(p->len) {
		while(tot < p->len) {
			debug_printf("%04X  ", tot);
			for(x=0; x<16; x++) {
				if(line+x < p->len) {
					tot++;
					debug_printf("%02X ", p->buffer[line+x]);
				}
				else debug_puts("   ");
				if(x == 7) debug_putc(' ');
			}
			debug_puts(" |");
			for(x=0; x<16; x++) {
				if(line+x < p->len) {
					uint8_t c = p->buffer[line+x];
					if(isprint(c)) debug_putc(c); else debug_putc(' ');
				} else debug_putc(' ');
			}
			debug_putc('|');
			debug_putcrlf();
			line = tot;
		}

	}
	debug_puts("--- end of dump ---"); debug_putcrlf();
}
