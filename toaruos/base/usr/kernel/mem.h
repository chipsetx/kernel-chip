/* vim: tabstop=4 shiftwidth=4 noexpandtab
 */

#pragma once

#include <kernel/types.h>

extern uintptr_t heap_end;

extern void set_frame(uintptr_t frame_addr);
extern void clear_frame(uintptr_t frame_addr);
extern uint32_t test_frame(uintptr_t frame_addr);
extern uint32_t first_frame(void);

extern uintptr_t map_to_physical(uintptr_t virtual);

