/* SPDX-License-Identifier: MIT */
/* Bounded caller-selected function export; no guest discovery or catalog. */
#include "backend/emitter.h"
#include "backend/dispatch.h"
#include "frontend/decoder.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

static FILE* exclusive_output(const char* path) {
#ifdef _WIN32
    /* Legacy MSVCRT rejects C11's fopen("wx"). _O_EXCL supplies the same
     * create-new guarantee without truncating an existing output. */
    const int fd=_open(path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
    if(fd<0)return NULL;
    FILE* output=_fdopen(fd,"wb");
    if(!output)_close(fd);
    return output;
#else
    return fopen(path,"wx");
#endif
}

int main(int argc,char** argv) {
    if(argc!=5 && argc!=6) {
        fprintf(stderr,"usage: dolrecomp-function-unit RAW-BE-PPC BASE gekko|broadway|espresso OUTPUT-C [SYMBOL-SUFFIX]\n");return 2;
    }
    char* end;errno=0;unsigned long base=strtoul(argv[2],&end,0);
    if(errno || end==argv[2] || *end || argv[2][0]=='-' || base>0xfffffffful || (base&3u))return 2;
    DolRecompCPU cpu;
    if(!strcmp(argv[3],"gekko"))cpu=DOLRECOMP_CPU_GEKKO;
    else if(!strcmp(argv[3],"broadway"))cpu=DOLRECOMP_CPU_BROADWAY;
    else if(!strcmp(argv[3],"espresso"))cpu=DOLRECOMP_CPU_ESPRESSO;
    else return 2;
    FILE* input=fopen(argv[1],"rb");if(!input) {perror("Cannot open function input");return 2;}
    if(fseek(input,0,SEEK_END)) {fclose(input);return 2;}
    long bytes=ftell(input);rewind(input);
    if(bytes<=0 || bytes%4 || bytes>1024*1024 || (u64)base+(u64)bytes>0xffffffffu) {fclose(input);return 2;}
    const u32 count=(u32)bytes/4u;PPCInst* code=calloc(count,sizeof(*code));
    if(!code) {fclose(input);return 2;}
    for(u32 i=0;i<count;++i) {
        u8 word[4];if(fread(word,1,4,input)!=4) {free(code);fclose(input);return 2;}
        code[i]=ppc_decode((u32)word[0]<<24|(u32)word[1]<<16|(u32)word[2]<<8|word[3],(u32)base+i*4u);
    }
    fclose(input);FILE* temporary=tmpfile();if(!temporary) {perror("Cannot create temporary function output");free(code);return 2;}
    if(argc==6)dolrecomp_set_symbol_suffix(argv[5]);
    emit_header_for_cpu(temporary,cpu);
    if(!emit_function_unit(temporary,code,count,(u32)base)) {free(code);fclose(temporary);return 1;}
    free(code);rewind(temporary);
    /* Do not overwrite a caller's retained generated artifact. */
    FILE* output=exclusive_output(argv[4]);if(!output) {perror("Cannot create function output");fclose(temporary);return 2;}
    char buffer[4096];size_t size;int failed=0;
    while((size=fread(buffer,1,sizeof buffer,temporary)))
        if(fwrite(buffer,1,size,output)!=size) {failed=1;break;}
    if(ferror(temporary))failed=1;
    if(fclose(output))failed=1;
    fclose(temporary);return failed?2:0;
}
