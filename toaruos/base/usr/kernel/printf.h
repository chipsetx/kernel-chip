#pragma once

#include <va_list.h>

#include <kernel/fs.h>

extern size_t vasprintf(char * buf, const char *fmt, va_list args);
extern int    sprintf(char *buf, const char *fmt, ...);
extern int    fprintf(fs_node_t * device, char *fmt, ...);

