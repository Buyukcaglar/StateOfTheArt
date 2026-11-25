/* dis <file> <file_offset> <load_addr> <length> [start_addr]: disassemble raw bytes */
#include "../../runtime/cpu/m68k.h"
#include <stdio.h>
#include <stdlib.h>
static uint8_t mem[1<<24];
uint32_t mem_fetch16(uint32_t a){a&=0xffffff;return mem[a]<<8|mem[a+1];}
uint32_t mem_rb(uint32_t a){return mem[a&0xffffff];}
uint32_t mem_rw(uint32_t a){return mem_fetch16(a);}
uint32_t mem_rl(uint32_t a){return mem_rw(a)<<16|mem_rw(a+2);}
void mem_wb(uint32_t a,uint32_t v){(void)a;(void)v;} void mem_ww(uint32_t a,uint32_t v){(void)a;(void)v;} void mem_wl(uint32_t a,uint32_t v){(void)a;(void)v;}
void m68k_on_reset_instruction(void){}
int main(int c,char**v){
  if(c<5){fprintf(stderr,"usage\n");return 1;}
  FILE*f=fopen(v[1],"rb"); long off=strtol(v[2],0,0); uint32_t at=strtoul(v[3],0,0); long len=strtol(v[4],0,0);
  fseek(f,off,SEEK_SET); fread(mem+at,1,len,f); fclose(f);
  uint32_t pc=c>5?strtoul(v[5],0,0):at;
  while(pc<at+len){Insn i; m68k_decode(pc,&i); char b[128]; m68k_disasm(&i,b,sizeof b);
    printf("%06x  ",pc); for(int k=0;k<6;k++) printf(k*2<i.len?"%04x ":"     ",mem_fetch16(pc+k*2)); printf(" %s\n",b); pc+=i.len;}
}
