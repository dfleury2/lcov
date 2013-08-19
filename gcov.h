#ifndef __LOCAL_GCOV_H_CPP_MIGRATION
#define __LOCAL_GCOV_H_CPP_MIGRATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>


#include <sys/types.h>
#include <sys/stat.h>
#ifdef WIN32
#include <direct.h>
#include <windows.h>
#include <tchar.h>
#else
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>
#include <errno.h>
#endif


// GCOV specific defines
#define IN_GCOV 1
#define HOST_WIDEST_INT long long
#define HOST_WIDEST_INT_PRINT_DEC "%ld"
#define gcc_assert(PRED) {}
#define xrealloc realloc
#define xcalloc calloc
#define xmalloc malloc
#define xstrdup strdup
#define _(MSG) MSG

#define FATAL_EXIT_CODE 1
#define SUCCESS_EXIT_CODE 0

const char* bug_report_url = "Error URL";
const char* version_string = "Version Number";

#endif
