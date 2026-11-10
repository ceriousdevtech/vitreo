#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef void* (__cdecl *MemCpy)(void*, const void*, size_t);
static void ref_memmove(unsigned char* d, const unsigned char* s, size_t n) {
    if (d < s) for (size_t i = 0; i < n; ++i) d[i] = s[i];
    else for (size_t i = n; i-- > 0;) d[i] = s[i];
}
int main(void){
 const char* dlls[]={"vcruntime140.dll","ucrtbase.dll","msvcrt.dll","ntdll.dll"};
 static unsigned char buf[12000], refb[12000];
 long deltas[]={1,3,8,17,64,1000,-1,-3,-8,-17,-64,-1000};
 size_t ns[]={1,3,8,16,31,64,100,256,1000,4096,5000};
 for(int d=0;d<4;++d){HMODULE h=LoadLibraryA(dlls[d]);MemCpy f=h?(MemCpy)GetProcAddress(h,"memcpy"):0;
  int bad=0,total=0;
  if(f)for(size_t di=0;di<sizeof deltas/sizeof*deltas;++di)for(size_t ni=0;ni<sizeof ns/sizeof*ns;++ni){
   for(int i=0;i<12000;++i)buf[i]=refb[i]=(unsigned char)(i*13+5);
   unsigned char*s=buf+3001,*dd=s+deltas[di];f(dd,s,ns[ni]);ref_memmove(refb+(dd-buf),refb+(s-buf),ns[ni]);
   ++total; if(memcmp(buf,refb,sizeof buf)!=0){++bad;printf("%s\tmismatch\tdelta=%ld\tn=%zu\n",dlls[d],deltas[di],ns[ni]);}}
  printf("%s\t%s\tmemmove_safe_cases=%d/%d\n",dlls[d],f?"present":"absent",total-bad,total);}
 return 0;}
