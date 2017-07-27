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
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>
#include <time.h>

#define MAXPATHLEN 256

FILE* open_output_file(char* dir, uint32_t pid)
{
	FILE*		 fd = NULL;
	char		 out_name[MAXPATHLEN];
	char		 fmt[128];
	time_t		 raw;
	struct tm	*tm;

	time(&raw);
	tm = localtime(&raw);

	strftime(fmt, 128, "%Y%m%d-%H%M%S", tm);
	sprintf(out_name, "%s/%s-pagemap-pid-%d.csv", dir, fmt, pid);
	
	fd = fopen(out_name, "w");
	if (fd == NULL)
	{
		fprintf(stderr, "error opening file: %s\n", strerror(errno));
		exit(100);
	}

	return fd;

}
FILE* open_pid_maps(uint32_t pid)
{
	FILE*	fd = NULL;
	char	maps_name[MAXPATHLEN];
	sprintf(maps_name, "/proc/%d/maps", pid);

	fd = fopen(maps_name, "r"); 
	if (fd == NULL) 
	{
		fprintf(stderr, "error opening maps: %s\n", strerror(errno));
		exit(101);
	}
	return fd;
}


int open_pid_pagemap(uint32_t pid)
{
	int	fd = -1;
	char	pmap_name[MAXPATHLEN];
	sprintf(pmap_name, "/proc/%d/pagemap", pid);

	fd = open(pmap_name, O_RDONLY);
	if (fd == -1)
	{
		fprintf(stderr, "error opening pagemap: %s\n", strerror(errno));
		exit(102);
	}
	return fd;
}

int get_mem_stripe(uint64_t addr)
{
	uint64_t i                 = 0x0000000020000000;
	uint64_t stripe            = 0x0;
	const uint64_t stripe_size = 0x0000000020000000;

	while (1)
	{
		if (addr < i) { break;    }
		if (addr > i) { ++stripe; }

		i += stripe_size;
	}
	return stripe;
}



#define is_bit_set(val, bit) ((val) & ((uint64_t)1 << (bit)))

int main(int argc, char* argv[])
{
	int		c;
	static uint32_t pid = 0;

	FILE*	of;
	FILE*	ma;
	int	pm;

	char* dir = "/mnt";

	while (1)
	{
		static struct option long_options[] = {
			{"pid",     required_argument, 0, 'p'},
			{0, 0, 0, 0}
		};
		int	option_index		    = 0;
		c				    = getopt_long (argc, argv, "d:p:",
		                 long_options, &option_index);

		if (c == -1) { break; }

		switch (c)
		{
		case 'p':
			pid = atoi(optarg);
			break;
		case 'd':
			dir = optarg;
			break;
			
		case '?':
			/* getopt_long already printed an error message. */
			break;

		default:
			abort ();
		}
	}
	if (pid == 0)
	{
		fprintf(stderr, "error pid not set = 0\n");
		exit(200);
	}
		
	
	of = open_output_file(dir, pid);
	ma = open_pid_maps(pid); 
	pm = open_pid_pagemap(pid); 


	fprintf(of, "virt-addr;page-present;page-swapped;file-page_or_shared-anon;z0;z1;z2;z3;"
		"page-exclusiv;soft-dirty;phys-addr;pagemap-entry;mem-stripe\n");

	
	const uint32_t	PAGEMAP_ENTRY_SIZE = 8;
	char		line[MAXPATHLEN];
	uint32_t	page_size	   = getpagesize();
	
	while (fgets(line, MAXPATHLEN, ma) != NULL)
	{
		uint64_t valid_range   = 0;
		uint64_t vm_addr_start = 0;
		uint64_t vm_addr_end   = 0;
		uint64_t vm_addr_diff  = 0;
		uint64_t num_of_pages  = 0;
				
		fprintf(of, "# %s\n", line);

		char del[] = " -";
				
		vm_addr_start = strtoll(strtok(line, del), NULL, 16);
		vm_addr_end   = strtoll(strtok(NULL, del), NULL, 16);

		fprintf(of, "# %llX-%llX page-size: %d\n", vm_addr_start, vm_addr_end, page_size);

		vm_addr_diff = vm_addr_end - vm_addr_start;

		if (vm_addr_diff < page_size)
		{
			fprintf(stderr, "warning: vm_addr_diff %" PRIu64 " < page_size %d\n", vm_addr_diff, page_size);
			continue;
		}
		
		num_of_pages = vm_addr_diff / page_size;

		if (num_of_pages < 0)
		{
			fprintf(stderr, "warning: num_of_pages  %" PRIu64 " < 0\n", num_of_pages);
			continue;

		}


		
		off64_t offset = (vm_addr_start / page_size) * PAGEMAP_ENTRY_SIZE;
		int64_t retval = lseek64(pm, offset, SEEK_SET);

		/*printf("%llX %lld %lld %lld\n", vm_addr_start, page_size, offset, retval);*/
		
		if (retval != offset)
		{
			fprintf(stderr, "error seeking pagemap: %s\n", strerror(errno));
			exit(203);
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
		uint32_t page_counter = 0;
		
		while (num_of_pages > 0)
		{
			
			uint64_t pa   = 0x0;
			uint64_t phys_addr = 0x0;
			ssize_t bytes = 0;
			

			bytes = read(pm, &pa, sizeof(uint64_t));

			if (bytes < 0)
			{
				fprintf(stderr, "error reading pagemap: %s\n", strerror(errno));
				exit(204);
			}


			phys_addr = (pa & 0x7FFFFFFF) * page_size;

			
			fprintf(of, "%016llX; %c; %c; %c; %c; %c; %c; %c; %c; %c; %016llX; %016llX; %d\n",
			       vm_addr_start + page_size * page_counter,
			       is_bit_set(pa, 63) ? '1' : '0',
			       is_bit_set(pa, 62) ? '1' : '0',
			       is_bit_set(pa, 61) ? '1' : '0',
			       '0',
			       '0',
			       '0',
			       '0',
			       is_bit_set(pa, 56) ? '1' : '0',
			       is_bit_set(pa, 55) ? '1' : '0',
				phys_addr,
				pa, 
				get_mem_stripe(phys_addr));
			

			num_of_pages--;
			page_counter++;
		}
		
	}
	
	
	
	fclose(of);
	fclose(ma);
	close(pm);

	
	return EXIT_SUCCESS;
}
