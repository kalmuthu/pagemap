/* pagemap -- show mapping of virtual to physical pages
 * 
 * Author:    Zvonko Kosic
 * Copyright: Zvonko Kosic
 * 
 * pagemap is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License
 * as published by the Free Software Foundation
 *
 * pagemap is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pagmap.  If not, see http://www.gnu.org/licenses. 
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>


#define MAXPATHLEN 256
#define is_bit_set(val, bit) ((val) & ((uint64_t)1 << (bit)))
#define cout_is_bit_set(val, bit) is_bit_set(val, bit) ? '1' : '0'


const uint32_t  PAGEMAP_ENTRY_SIZE = 8;
static uint32_t page_size          = 0;
static uint32_t page_shift         = 0;

uint32_t count_consecutive_zero_bits(uint64_t v)
{
	uint32_t c = 0;
	
	if (v)
	{
		v = (v ^ (v - 1)) >> 1;  /* Set v's trailing 0s to 1s and zero rest */
		for (c = 0; v; c++)
		{
			v >>= 1;
		}
	}
	else
	{
		c = CHAR_BIT * sizeof(v);
	}


	return c;
}

FILE* open_output_file(const uint32_t pid)
{
	FILE* fd = NULL;
	char out_name[MAXPATHLEN];
	sprintf(out_name, "./pagemap-%d.txt", pid);
	
	fd = fopen(out_name, "w");
	if (fd == NULL)
	{
		fprintf(stderr, "error opening file: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	return fd;

}
FILE* open_pid_maps(const uint32_t pid)
{
	FILE* fd = NULL;
	char maps_name[MAXPATHLEN];
	sprintf(maps_name, "/proc/%d/maps", pid);

	fd = fopen(maps_name, "r"); 
	if (fd == NULL) 
	{
		fprintf(stderr, "error opening maps: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	return fd;
}


int open_pid_pagemap(const uint32_t pid)
{
	int fd = -1;
	char pmap_name[MAXPATHLEN];
	sprintf(pmap_name, "/proc/%d/pagemap", pid);

	fd = open(pmap_name, O_RDONLY);
	if (fd == -1)
	{
		fprintf(stderr, "error opening pagemap: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	return fd;
}

void parse_cmdline(const int argc,
                   const char* const __restrict argv[],
                   uint32_t*         __restrict pid)
{
	int32_t c;
	while (1)
	{
		static struct option long_options[] = {
			{"pid",     required_argument, 0, 'p'},
			{0, 0, 0, 0}
		};
		int32_t option_index = 0;
		c = getopt_long (argc, argv, "o:p:",
		                 long_options, &option_index);

		if (c == -1) { break; }

		switch (c)
		{
		case 'p':
			*pid = atoi(optarg);
			break;
			
		case '?':
			/* getopt_long already printed an error message. */
			break;

		default:
			abort ();
		}
	}
	
	if (*pid == 0)
	{
		fprintf(stderr, "error pid not set = 0\n");
		exit(200);
	}
}

/* /proc/13632/maps --- A file containing the currently mapped memory regions
 * and their access permissions.  See mmap(2) for some further information
 * about memory mappings.
 *
 * 00400000-0062b000 r-xp 00000000 fc:01 5776229  /home/zkosic/bin/emacs-24.5
 * 0082a000-0082b000 r--p 0022a000 fc:01 5776229  /home/zkosic/bin/emacs-24.5
 * 0082b000-0135b000 rw-p 0022b000 fc:01 5776229  /home/zkosic/bin/emacs-24.5
 * 0296d000-09fb9000 rw-p 00000000 00:00 0        [heap]
 * ----------------------------%<---%<---------------------------------------- 
*/
off64_t traverse_maps_for_offsets(char*     __restrict line,
                                  uint64_t* __restrict num_pages,
                                  uint64_t* __restrict vm_addr_beg)
{
	uint64_t valid_range  = 0;
	uint64_t vm_addr_end  = 0;
	uint64_t vm_addr_diff = 0;
	off64_t  offset       = 0;
	char*    beg          = NULL;
	char*    end          = NULL;
	
	fprintf(stdout, "%s\n", line);

	beg = strtok(line, " -");
	end = strtok(NULL, " -");

	*vm_addr_beg = strtoull(beg, NULL, 16);
	vm_addr_end    = strtoull(end, NULL, 16);
	
	vm_addr_diff = vm_addr_end - *vm_addr_beg;

	if (vm_addr_diff < page_size)
	{
		fprintf(stderr, "warning: vm_addr_diff %" PRIu64 " < page_size %d\n", vm_addr_diff, page_size);
		return -1;
	}
		
	*num_pages = vm_addr_diff >> page_shift;

	if (*num_pages == 0)
	{
		fprintf(stderr, "warning: *num_pages  %" PRIu64 " < 0\n", *num_pages);
		return -1;

	}

	offset = (*vm_addr_beg / page_size) * PAGEMAP_ENTRY_SIZE;
		
	return offset;
}
/* /proc/pid/pagemap.  This file lets a userspace process find out which
 * physical frame each virtual page is mapped to.  It contains one 64-bit
 * value for each virtual page, containing the following data (from
 * fs/proc/task_mmu.c, above pagemap_read):
 *
 *    * Bits 0-54  page frame number (PFN) if present
 *    * Bits 0-4   swap type if swapped
 *    * Bits 5-54  swap offset if swapped
 *    * Bit  55    pte is soft-dirty (see Documentation/vm/soft-dirty.txt)
 *    * Bit  56    page exclusively mapped (since 4.2)
 *    * Bits 57-60 zero
 *    * Bit  61    page is file-page or shared-anon (since 3.5)
 *    * Bit  62    page swapped 
 *    * Bit  63    page present 
 */
void cout_pagemap_entry_details(const uint64_t phys,
                                const uint64_t vm_addr_beg,
                                const uint32_t page_counter)
{
	uint64_t vm_addr = vm_addr_beg + page_size * page_counter;
	
	printf("%016llX -> %c %c %c %c %c %c %c %c %c %016llX (%016llX)\n",
	       vm_addr,
	       cout_is_bit_set(phys, 63),
	       cout_is_bit_set(phys, 62),
	       cout_is_bit_set(phys, 61),
	       '0', '0', '0', '0',           
	       cout_is_bit_set(phys, 56),
	       cout_is_bit_set(phys, 55),
	       (phys & 0x7FFFFFFF) * page_size,
	       phys);
	
}


void traverse_pagemap_for_phys_pfn(const int      pm,
                                   const off64_t  offset,
                                   const uint64_t vm_addr_beg,
                                   const uint32_t num_pages)
{
	uint32_t page_counter = 0;
	int64_t  retval       = lseek64(pm, offset, SEEK_SET);
	
	if (retval != offset)
	{
		fprintf(stderr, "error seeking pagemap: %s\n", strerror(errno));
		exit(203);
	}
		
	while (page_counter < num_pages)
	{
		uint64_t phys    = 0x0;
		ssize_t  bytes   = 0;

		bytes = read(pm, &phys, sizeof(uint64_t));

		if (bytes < 0)
		{
			fprintf(stderr, "error reading pagemap: %s\n", strerror(errno));
			exit(204);
		}

		cout_pagemap_entry_details(phys, vm_addr_beg, page_counter);
		
		page_counter++;
	}
		
}


int main(int argc, char* argv[])
{
	uint32_t pid = 0;

	FILE* of;
	FILE* ma;
	int   pm;
	

	char line[MAXPATHLEN];
	uint64_t num_pages = 0;

	page_size = getpagesize();

	page_shift = count_consecutive_zero_bits(page_size);

	printf("page shift: %d\n", page_shift);

	parse_cmdline(argc, argv, &pid);
	
	of = open_output_file(pid);
	ma = open_pid_maps(pid); 
	pm = open_pid_pagemap(pid); 

	
	while (fgets(line, MAXPATHLEN, ma) != NULL)
	{
		uint64_t vm_addr_beg = 0;
		off64_t offset = traverse_maps_for_offsets(line, &num_pages, &vm_addr_beg);
		traverse_pagemap_for_phys_pfn(pm, offset, vm_addr_beg, num_pages);
	}
	
	fclose(of);
	fclose(ma);
	close(pm);
	
	return EXIT_SUCCESS;
}
