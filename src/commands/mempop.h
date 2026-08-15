#ifndef MEMPOP_H
#define MEMPOP_H

#include <stdint.h>
#include <stdbool.h>
#define MAX_ALLOCATIONS 1000
static void* allocations[MAX_ALLOCATIONS];
static uint32_t allocation_sizes[MAX_ALLOCATIONS];
static uint32_t allocation_ids[MAX_ALLOCATIONS];
static int allocation_count = 0;
static uint32_t next_alloc_id = 1;

void mempop_command(int argc, char* argv[]);


#endif // MEMPOP_H
