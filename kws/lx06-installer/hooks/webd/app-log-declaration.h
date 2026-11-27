/* Read declarations as data, never source/eval an uploaded shell script.
 * Scan only the first 32 KiB; embedded binary payloads are irrelevant. */
#ifndef LX06_APP_LOG_DECLARATION_H
#define LX06_APP_LOG_DECLARATION_H
#include <stdio.h>
#include <string.h>
#include <ctype.h>
struct app_log_var { char key[64], value[512]; };
static int app_log_expand(const char *s, struct app_log_var *vars, int count,
                          char *out, size_t cap) {
  size_t n=0; char quote=0;
  while (*s==' ' || *s=='\t') s++;
  while (*s && *s!='\r' && *s!='\n') {
    char c=*s++;
    if (!quote && (c==' ' || c=='\t')) {
      while (*s==' ' || *s=='\t') s++;
      if (*s && *s!='#' && *s!='\r' && *s!='\n') return -1;
      break;
    }
    if (!quote && c=='#' && !n) break;
    if (c=='\'' || c=='"') {
      if (!quote) { quote=c; continue; }
      if (quote==c) { quote=0; continue; }
    }
    if (c=='$' && quote!='\'') {
      char key[64]; size_t k=0; int braced=*s=='{', i;
      if (braced) s++;
      if (!(isalpha((unsigned char)*s) || *s=='_')) return -1;
      while (isalnum((unsigned char)*s) || *s=='_') {
        if (k+1>=sizeof(key)) return -1;
        key[k++]=*s++;
      }
      key[k]=0;
      if (braced && *s++!='}') return -1;
      for (i=count-1;i>=0;i--) if (!strcmp(key,vars[i].key)) break;
      if (i<0 || n+strlen(vars[i].value)>=cap) return -1;
      strcpy(out+n,vars[i].value); n+=strlen(vars[i].value);
      continue;
    }
    /* No shell commands, escapes, substitutions, globbing or control bytes. */
    if (strchr("`\\;|&<>()*?!",c) || (unsigned char)c<32 || n+1>=cap) return -1;
    out[n++]=c;
  }
  if (quote || !n) return -1;
  out[n]=0;
  return 1;
}
static int app_log_declaration(FILE *f, char *out, size_t cap) {
  struct app_log_var vars[32]; int count=0, result=0;
  char line[1024], value[512]; size_t consumed=0;
  while (consumed<32768 && fgets(line,sizeof(line),f)) {
    size_t len=strlen(line); consumed+=len;
    if (!strchr(line,'\n') && !feof(f)) {
      /* Extension JSON or embedded payloads may exceed the line buffer.
       * Discard the WHOLE physical line, then continue within the same scan
       * budget. Never parse its continuation as a new shell declaration. */
      while (consumed<32768 && fgets(line,sizeof(line),f)) {
        consumed+=strlen(line);
        if (strchr(line,'\n')) break;
      }
      continue;
    }
    if (consumed>32768) break;
    char *s=line, key[64]; size_t k=0; int i;
    while (*s==' ' || *s=='\t') s++;
    if (!strncmp(s,"# LX06-LOGFILE:",15)) {
      if (app_log_expand(s+15,vars,count,value,sizeof(value))<0 || strlen(value)>=cap) return -1;
      strcpy(out,value); return 1;
    }
    if (!strncmp(s,"export ",7)) { s+=7; while (*s==' ' || *s=='\t') s++; }
    if (!(isalpha((unsigned char)*s) || *s=='_')) continue;
    while (isalnum((unsigned char)*s) || *s=='_') {
      if (k+1>=sizeof(key)) break;
      key[k++]=*s++;
    }
    key[k]=0;
    if (*s++!='=') continue;
    int valid=app_log_expand(s,vars,count,value,sizeof(value));
    if (!strcmp(key,"logfile")) {
      if (valid<0 || strlen(value)>=cap) return -1;
      strcpy(out,value); return 1;
    }
    for (i=0;i<count;i++) if (!strcmp(key,vars[i].key)) break;
    if (valid<0) { /* Do not resolve a later reference to a stale value. */
      if (i<count) vars[i]=vars[--count];
      continue;
    }
    if (i>=32) continue;
    if (i==count) count++;
    strcpy(vars[i].key,key); strcpy(vars[i].value,value);
  }
  return result;
}
#endif
