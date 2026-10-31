// Host helper: runs one nu_sql.h function on stdin and prints the result (used by nu_sqlite_check.py).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nu_sql.h"
static char in[200000],out[400000];
int main(int argc,char **argv){
    size_t n=fread(in,1,sizeof in-1,stdin);in[n]=0;
    while(n&&(in[n-1]=='\n'||in[n-1]=='\r'))in[--n]=0;
    NuCfg cfg={1,{0,3000,6000,12000},{10,12,14,16},5};
    if(argc>2)cfg.area=atoi(argv[2]);
    NuBuf b;nu_begin(&b,out,sizeof out-1);
    const char *m=argc>1?argv[1]:"";
    long ord=0,garage=0;int lvl=0,ok=0;
    if(!strcmp(m,"dbsql"))ok=nu_database_sql(&b,&cfg);
    else if(!strcmp(m,"view"))ok=nu_view(in,n,&b,&ord,&cfg);
    else if(!strcmp(m,"partlist")){if(!nu_partlist_enum(in,n,&garage)){puts("NOENUM");return 2;}ok=nu_partlist(in,n,&b,atoi(argv[2]));}
    else if(!strcmp(m,"update"))ok=nu_update(in,n,&b,&lvl,&garage);
    else if(!strcmp(m,"stock"))return nu_stock_check(in,n)?0:1;
    else if(!strcmp(m,"text")){char t[100];const char *s=nu_key_text((unsigned)atoi(argv[2]),t,&cfg);if(!s)return 1;puts(s);return 0;}
    if(!ok){puts("NOCHANGE");return 1;}
    fwrite(out,1,b.at,stdout);fputc('\n',stdout);
    if(!strcmp(m,"update"))fprintf(stderr,"level=%d garage=%ld\n",lvl,garage);
    return 0;
}
