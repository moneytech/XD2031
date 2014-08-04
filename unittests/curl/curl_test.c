
/****************************************************************************

    Serial line filesystem server
    Copyright (C) 2012 Andre Fachat

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

#include <stdlib.h>
#include <stdio.h>

#include "provider.h"
#include "wireformat.h"

extern provider_t ftp_provider;
extern provider_t http_provider;

void ftp_test();
void http_test();

void get_test(provider_t *prov, const char *rootpath, const char *name);
void dir_test(provider_t *prov, const char *rootpath, const char *name);

int main(int argc, char *argv[]) {

	ftp_test();

	http_test();

}


void ftp_test() {

	provider_t *prov = &ftp_provider;

	//get_test(prov, "anonymous:fachat_at_web.de@ftp.zimmers.net/pub/cbm", "00INDEX");
	//get_test(prov, "ftp.zimmers.net/pub/cbm", "00INDEX");
	get_test(prov, "ftp.zimmers.net/pub/cbm", "index.html");

	//dir_test(prov, "ftp.zimmers.net/pub/cbm", "");
}

void http_test() {

	provider_t *prov = &http_provider;

	get_test(prov, "www.zimmers.net/anonftp/pub/cbm", "index.html");
}

void dir_test(provider_t *prov, const char *rootpath, const char *name) {

	printf("dirprov=%p\n", prov);

	prov->init();

	printf("prov init done\n");

	// get endpoint for base URL
	endpoint_t *ep = prov->newep(rootpath);

	printf("ep=%p\n", ep);

	int rv = prov->opendir(ep, 1, name);

	printf("open -> %d\n", rv);

	char *buffer = malloc(8192 * sizeof(char));
	int eof = 0;
	do {
		rv = prov->readfile(ep, 1, buffer, 100, &eof);

		if (rv != 0) printf("readfile -> %d, eof=%d\n", rv, eof);

		if (rv > 0) {
			printf("---> %d: ", rv);
			for (int i = 0; i < FS_DIR_NAME; i++) {
				printf("%02x ", buffer[i]);
			}
			printf(": %s\n", buffer+FS_DIR_NAME);
		}
	}
	while (rv >= 0 && eof == 0);

	free(buffer);
	
	prov->close(ep, 1);

}

void get_test(provider_t *prov, const char *rootpath, const char *name) {

	printf("prov=%p\n", prov);

	prov->init();

	printf("prov init done\n");

	// get endpoint for base URL
	endpoint_t *ep = prov->newep(rootpath);

	printf("ep=%p\n", ep);

	int rv = prov->open_rd(ep, 1, name);

	printf("open -> %d\n", rv);

	char *buffer = malloc(8192 * sizeof(char));
	int eof = 0;
	do {
		rv = prov->readfile(ep, 1, buffer, 100, &eof);

		if (rv != 0) printf("readfile -> %d, eof=%d\n", rv, eof);

		if (rv > 0) {
			buffer[rv] = 0;
			printf("---> %s\n", buffer);
		}
	}
	while (rv >= 0 && eof == 0);

	free(buffer);
	
	prov->close(ep, 1);

}
