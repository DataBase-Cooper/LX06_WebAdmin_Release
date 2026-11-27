#include <string.h>
size_t strlcpy(char *d, const char *s, size_t n){ size_t l=strlen(s); if(n){size_t c=n-1; if(l<c)c=l; memcpy(d,s,c); d[c]=0;} return l; }
size_t strlcat(char *d, const char *s, size_t n){ size_t dl=strlen(d), l=strlen(s); if(n){ size_t c=n-dl-1; if(c<1)c=0; if(l>c)l=c; memcpy(d+dl,s,l); } return dl+l; }
