/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_APP_NATIVE_FUNCTION_H
#define DOLRECOMP_APP_NATIVE_FUNCTION_H
#include <stdio.h>

int dr_native_function_main(int argc, char** argv);
int dr_native_function_rel_main(int argc,char** argv);
FILE* dr_native_open_output(const char* directory,const char* name);

#endif
