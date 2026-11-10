#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef int (__cdecl *MemCmp)(const void*, const void*, size_t);
int main(void){
 const char* dlls[]={"vcruntime140.dll","ucrtbase.dll","msvcrt.dll","ntdll.dll"};
 static unsigned char a[64],b[64];
 unsigned char pairs[][2]={{0x01,0x80},{0x80,0x01},{0x00,0xFF},{0x41,0x42},{0x42,0x41},{0x10,0x30}};
 size_t ns[]={1,4,8,16,33};
 for(int d=0;d<4;++d){HMODULE h=LoadLibraryA(dlls[d]);MemCmp f=h?(MemCmp)GetProcAddress(h,"memcmp"):0;
  printf("%s\t%s",dlls[d],f?"":"absent");
  if(f)for(int k=0;k<6;++k)for(int n=0;n<5;++n){memset(a,7,64);memset(b,7,64);a[ns[n]-1]=pairs[k][0];b[ns[n]-1]=pairs[k][1];printf("\t%d",f(a,b,ns[n]));}
  printf("\n");}
 return 0;}
