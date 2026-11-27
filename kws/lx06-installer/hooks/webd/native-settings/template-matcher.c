/* Private host analysis only. Not a new approved product capacity. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "revision11 capacity experiment is fixture-only"
#endif
#include "template-matcher.h"
#include <stdio.h>
#include <string.h>

static int utf8_len(const unsigned char *s) {
    unsigned c=s[0]; int n;
    if(c<128)return c?1:0;
    if(c>=0xc2 && c<=0xdf)n=2;
    else if(c>=0xe0 && c<=0xef)n=3;
    else if(c>=0xf0 && c<=0xf4)n=4;
    else return 0;
    for(int i=1;i<n;i++)if(!s[i] || (s[i]&0xc0)!=0x80)return 0;
    if((c==0xe0 && s[1]<0xa0)||(c==0xed && s[1]>=0xa0)||
       (c==0xf0 && s[1]<0x90)||(c==0xf4 && s[1]>=0x90))return 0;
    return n;
}

struct raw_map { const char *text; int start[513], end[513]; };
static int mapped_normalize(const char *text,char *norm,struct raw_map *map) {
    int at=0,n=0,chars=0;
    map->text=text;
    while(text[at]) {
        int k=utf8_len((const unsigned char *)text+at);
        if(!k || ++chars>128)return 0;
        unsigned cp=(unsigned char)text[at];
        if(k>1) {
            cp &= (1u << (7-k))-1;
            for(int j=1;j<k;j++)cp=(cp<<6)|((unsigned char)text[at+j]&63);
        }
        int space=(cp>=9 && cp<=13) || cp==32 || cp==0x85 || cp==0xa0 ||
            cp==0x1680 || (cp>=0x2000 && cp<=0x200a) || cp==0x2028 || cp==0x2029 ||
            cp==0x202f || cp==0x205f || cp==0x3000;
        if(!space) {
            if((unsigned char)text[at]<32 || n+k>512)return 0;
            for(int j=0;j<k;j++) { norm[n]=text[at+j];map->start[n]=at;map->end[n]=at+k;n++; }
        }
        at+=k;
    }
    norm[n]=0;
    /* Only sentence-final punctuation is ignored; internal punctuation stays. */
    while(n) {
        int begin=n-1;while(begin>0 && ((unsigned char)norm[begin]&0xc0)==0x80)begin--;
        int mark=(n-begin==1 && strchr(".!?,",norm[begin])) ||
            (n-begin==3 && (!memcmp(norm+begin,"。",3)||!memcmp(norm+begin,"！",3)||!memcmp(norm+begin,"？",3)||!memcmp(norm+begin,"，",3)));
        if(!mark)break;
        norm[n=begin]=0;
    }
    return n>0;
}
int lx06_phrase_normalize(const char *in,char *out,size_t cap) {
    char norm[513];struct raw_map map;
    if(!out || !cap)return 0;
    if(!in || !mapped_normalize(in,norm,&map) || strlen(norm)>=cap) {out[0]=0;return 0;}
    strcpy(out,norm);return 1;
}

static int char_count(const char *s, int bytes) {
    int n = 0, i = 0;
    if (bytes < 0) return -1;
    while (i < bytes) {
        int k = utf8_len((const unsigned char *)s + i);
        if (k <= 0 || i + k > bytes) return -1;
        i += k;
        n++;
    }
    return n;
}

static int var_limit(const char *name) {
    if (!strcmp(name, "artist") || !strcmp(name, "song")) return 32;
    return 64;
}

#include "template-runtime.h"

struct lx06_seg {
    int var;
    char lit[513];
    int lit_bytes;
    char name[17];
    int vmin, vmax;
};

struct lx06_pat {
    int ok, segs, vars, fixed;
    char skeleton[513];
    struct lx06_seg seg[9];
};

static int script_same(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    return !strcmp(a, b);
}

static int parse_pat(const char *phrase, struct lx06_pat *pat) {
    const char *p = phrase;
    char norm[513];
    size_t n = 0;
    memset(pat, 0, sizeof(*pat));
    if (!phrase || !*phrase || strlen(phrase) >= sizeof(norm)) return 0;
    if(!lx06_phrase_normalize(phrase,norm,sizeof(norm)))return 0;
    n=strlen(norm);
    if (!n) return 0;
    p = norm;
    while (*p) {
        struct lx06_seg *seg;
        if (pat->segs == 9) return 0;
        seg = &pat->seg[pat->segs++];
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if(!e || e==p+1)return 0;
            size_t name_n = (size_t)(e - (p + 1));
            if(name_n>16)return 0;
            for(size_t j=0;j<name_n;j++)
                if(!((p[1+j]>='a' && p[1+j]<='z') || p[1+j]=='_'))return 0;
            seg->var = 1;
            memcpy(seg->name, p + 1, name_n);
            seg->name[name_n] = 0;
            seg->vmin = 1;
            seg->vmax = var_limit(seg->name);
            pat->vars++;
            p = e + 1;
        } else {
            size_t k = 0;
            seg->var = 0;
            while (p[k] && p[k] != '{') k++;
            if (k >= sizeof(seg->lit)) return 0;
            memcpy(seg->lit, p, k);
            seg->lit[k] = 0;
            seg->lit_bytes = (int)k;
            {
                int chars = char_count(seg->lit, seg->lit_bytes);
                if (chars < 0) return 0;
                pat->fixed += chars;
                if (strlen(pat->skeleton) + k >= sizeof(pat->skeleton)) return 0;
                strcat(pat->skeleton, seg->lit);
            }
            p += k;
        }
    }
    if (pat->vars && pat->fixed <= 0) return 0;
    if (pat->vars > 4) return 0;
    pat->ok = 1;
    return 1;
}

static int capture_raw(struct lx06_match_var *var,const struct raw_map *map,int at,int end,const struct lx06_seg *seg) {
    if(end<=at)return 0;
    int start=map->start[at],bytes=map->end[end-1]-start;
    if(bytes>256)return 0; /* normalized length is checked by match_from */
    memset(var,0,sizeof(*var));strcpy(var->name,seg->name);
    memcpy(var->value,map->text+start,(size_t)bytes);return 1;
}
struct lx06_match_walk {
    unsigned *remaining;
    int *exhausted;
    int (*accept)(void *, const struct lx06_match_var *, int, const unsigned char *);
    void *ctx;
    unsigned char coverage[513];
};
static int match_from(const char *text, int at, int bytes, const struct lx06_pat *pat, int si,
                      struct lx06_match_var *vars, int *var_count, const struct raw_map *map, struct lx06_match_walk *walk) {
    const struct lx06_seg *seg;
    if(walk) {
        if(*walk->exhausted) return 0;
        if(!*walk->remaining) { *walk->exhausted=1; return 0; }
        --*walk->remaining;
    }
    if (si == pat->segs) {
        if(at!=bytes) return 0;
        return walk?walk->accept(walk->ctx,vars,*var_count,walk->coverage):1;
    }
    seg = &pat->seg[si];
    if (!seg->var) {
        if (at + seg->lit_bytes > bytes || memcmp(text + at, seg->lit, (size_t)seg->lit_bytes)) return 0;
        if(walk) memset(walk->coverage+at,1,(size_t)seg->lit_bytes);
        int rc=match_from(text, at + seg->lit_bytes, bytes, pat, si + 1, vars, var_count, map, walk);
        if(walk) memset(walk->coverage+at,0,(size_t)seg->lit_bytes);
        return rc;
    }
    if (si + 1 == pat->segs) {
        int chars = char_count(text + at, bytes - at);
        if (chars < seg->vmin || chars > seg->vmax) return 0;
        if (!capture_raw(&vars[*var_count],map,at,bytes,seg)) return 0;
        (*var_count)++;
        if(!walk) return 1;
        int rc=match_from(text,bytes,bytes,pat,si+1,vars,var_count,map,walk);
        --*var_count;
        return rc;
    }
    {
        int taken = 0, i = at;
        while (i < bytes && taken < seg->vmax) {
            int k = utf8_len((const unsigned char *)text + i);
            struct lx06_match_var saved[4];
            int saved_n;
            if (k <= 0 || i + k > bytes) return 0;
            i += k;
            taken++;
            if (taken < seg->vmin) continue;
            saved_n = *var_count;
            memcpy(saved, vars, sizeof(saved));
            if (!capture_raw(&vars[*var_count],map,at,i,seg)) continue;
            (*var_count)++;
            if (match_from(text, i, bytes, pat, si + 1, vars, var_count, map, walk)) return 1;
            *var_count = saved_n;
            memcpy(vars, saved, sizeof(saved));
        }
    }
    return 0;
}

#include "template-expression.inc"
#include "template-runtime.inc"

static int expr_match_inner(const char *text, const struct lx06_match_candidate *cands,
                        int count, struct lx06_match_result *out, int *branch, unsigned *remaining,
                        const unsigned char *exact) {
    char norm[513];
    struct raw_map map;
    int i, best_fixed = -1, best_index = -1, best_line = 0, scripts = 0;
    const char *best_script = "";
    struct lx06_match_var best_vars[4];
    int best_var_count = 0, incomplete = 0, best_branch = -1;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->index = -1;
    if (!text || count < 0 || (count > 0 && !cands)) return -1;
    if (count > 1024) return -1;
    if (!mapped_normalize(text, norm, &map)) return 0;
    for (i = 0; i < count; i++) {
        char phrase[513];
        struct raw_map pm;
        if (!exact[i]) continue;
        if (!mapped_normalize(cands[i].phrase, phrase, &pm)) continue;
        if (strcmp(phrase, norm)) continue;
        if (best_index < 0 || !script_same(cands[i].script, best_script)) {
            if (best_index >= 0 && !script_same(cands[i].script, best_script)) scripts++;
            else scripts = 1;
        }
        if (best_index < 0 || !script_same(cands[i].script, best_script) || cands[i].line < best_line) {
            /* Keep the first script's earliest line until a second script appears. */
            if (best_index < 0 || script_same(cands[i].script, best_script)) {
                best_index = i;
                best_line = cands[i].line;
                best_script = cands[i].script ? cands[i].script : "";
            }
        }
        if (scripts > 1) {
            out->kind = LX06_MATCH_AMBIGUOUS;
            out->index = -1;
            return 0;
        }
    }
    if (best_index >= 0 && scripts == 1) {
        out->kind = LX06_MATCH_EXACT;
        out->index = best_index;
        return 0;
    }
    best_index = -1;
    best_fixed = -1;
    scripts = 0;
    best_script = "";
    for (i = 0; i < count; i++) {
        struct expr_runtime_hit pat={0};
        if (exact[i]) continue;
        int rc=expr_runtime_match(cands[i].phrase,norm,&map,remaining,&pat);
        if(rc) return rc;
        if (pat.fixed<0) {
            if(pat.incomplete) incomplete=1;
            continue;
        }
        int var_count=pat.var_count;
        struct lx06_match_var *vars=pat.vars;
        if (pat.fixed > best_fixed) {
            best_fixed = pat.fixed;
            best_index = i;
            best_line = cands[i].line;
            best_script = cands[i].script ? cands[i].script : "";
            best_var_count = var_count;
            memcpy(best_vars, vars, sizeof(best_vars));
            best_branch=pat.branch;
            scripts = 1;
        } else if (pat.fixed == best_fixed) {
            if (!script_same(cands[i].script, best_script)) {
                scripts++;
            } else if (cands[i].line < best_line) {
                best_index = i;
                best_line = cands[i].line;
                best_var_count = var_count;
                memcpy(best_vars, vars, sizeof(best_vars));
                best_branch=pat.branch;
            }
        }
    }
    if (scripts > 1) {
        out->kind = LX06_MATCH_REJECT;
        out->fixed_chars = best_fixed;
        snprintf(out->prompt, sizeof(out->prompt), "%s", LX06_PROMPT_REJECT);
        return 0;
    }
    if (best_index >= 0) {
        out->kind = LX06_MATCH_TEMPLATE;
        out->index = best_index;
        out->fixed_chars = best_fixed;
        out->var_count = best_var_count;
        memcpy(out->vars, best_vars, sizeof(out->vars));
        *branch=best_branch;
        return 0;
    }
    if (incomplete) {
        out->kind = LX06_MATCH_INCOMPLETE;
        snprintf(out->prompt, sizeof(out->prompt), "%s", LX06_PROMPT_REASK);
        return 0;
    }
    out->kind = LX06_MATCH_NONE;
    return 0;
}


int lx06_expr_template_match(const char *text, const struct lx06_match_candidate *cands,
                             int count, struct lx06_match_result *out, int *branch, unsigned budget) {
    if(!text || !out || !branch || count<0 || count>1024 || (count && !cands) ||
       !budget || budget>EXPR_MATCH_STEPS) return EXPR_INVALID;
    /* Bounded copy also protects UTF-8 decoding at a caller guard page. */
    size_t n=expr_runtime_strlen(text,513);
    if(n>512) return EXPR_CAPACITY;
    char copy[513]; memcpy(copy,text,n); copy[n]=0;
    if(!expr_valid_utf8(copy,n)) return EXPR_INVALID;
    unsigned char exact[1024];
    int rc=expr_runtime_validate(cands,count,exact);
    if(rc) return rc;
    struct lx06_match_result next; int selected=-1;
    rc=expr_match_inner(copy,cands,count,&next,&selected,&budget,exact);
    memset(copy,0,sizeof(copy));
    if(!rc) { *out=next; *branch=selected; }
    return rc;
}
int lx06_template_match(const char *text, const struct lx06_match_candidate *cands,
                        int count, struct lx06_match_result *out) {
    int branch;
    return lx06_expr_template_match(text,cands,count,out,&branch,EXPR_MATCH_STEPS);
}
