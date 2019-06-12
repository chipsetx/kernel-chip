#pragma once

#include <kernel/types.h>

typedef struct page {
	unsigned int present:1;
	unsigned int rw:1;
	unsigned int user:1;
	unsigned int writethrough:1;
	unsigned int cachedisable:1;
	unsigned int accessed:1;
	unsigned int dirty:1;
	unsigned int pat:1;
	unsigned int global:1;
	unsigned int unused:3;
	unsigned int frame:20;
} __attribute__((packed)) page_t;

typedef struct page_table {
	page_t pages[1024];
} page_table_t;

typedef struct page_directory {
	uintptr_t physical_tables[1024];	/* Physical addresses of the tables */
	page_table_t *tables[1024];	/* 1024 pointers to page tables... */
	uintptr_t physical_address;	/* The physical address of physical_tables */
	int32_t ref_count;
} page_directory_t;

