#include "unicode-nfc.h"
#include <stddef.h>
#include <stdint.h>
#include "unicode-nfc-data.inc"

#define NFC_NAME_SCALARS 32u
#define NFC_NAME_BYTES 128u
#define NFC_WORK_SCALARS (NFC_NAME_SCALARS * NFC_CANONICAL_EXPANSION)
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))

static unsigned nfc_ccc(uint32_t codepoint){
    size_t lo=0,hi=COUNT(nfc_combining_classes);
    while(lo<hi){size_t middle=lo+(hi-lo)/2;uint32_t value=nfc_combining_classes[middle];
        if((value>>8)<codepoint)lo=middle+1;else hi=middle;}
    return lo<COUNT(nfc_combining_classes)&&(nfc_combining_classes[lo]>>8)==codepoint?
        nfc_combining_classes[lo]&255u:0;
}
static int nfc_no(uint32_t codepoint){
    size_t lo=0,hi=COUNT(nfc_quick_no);
    while(lo<hi){size_t middle=lo+(hi-lo)/2;
        if(nfc_quick_no[middle].last<codepoint)lo=middle+1;else hi=middle;}
    return lo<COUNT(nfc_quick_no)&&nfc_quick_no[lo].first<=codepoint;
}
static const struct nfc_mapping *nfc_mapping(uint32_t codepoint){
    size_t lo=0,hi=COUNT(nfc_mappings);
    while(lo<hi){size_t middle=lo+(hi-lo)/2;
        if(nfc_mappings[middle].codepoint<codepoint)lo=middle+1;else hi=middle;}
    return lo<COUNT(nfc_mappings)&&nfc_mappings[lo].codepoint==codepoint?nfc_mappings+lo:NULL;
}
static int nfc_emit(uint32_t codepoint,uint32_t *out,size_t *count){
    if(*count>=NFC_WORK_SCALARS)return 0;
    size_t position=(*count)++;unsigned ccc=nfc_ccc(codepoint);
    /* Stable canonical ordering; a starter (CCC=0) stops this insertion. */
    while(ccc&&position&&nfc_ccc(out[position-1])>ccc){out[position]=out[position-1];position--;}
    out[position]=codepoint;return 1;
}
static int nfc_decompose(uint32_t codepoint,unsigned depth,uint32_t *out,size_t *count){
    if(depth>NFC_CANONICAL_DEPTH)return 0;
    if(codepoint>=0xac00u&&codepoint<0xac00u+11172u){
        uint32_t index=codepoint-0xac00u;
        return nfc_emit(0x1100u+index/588u,out,count)&&
            nfc_emit(0x1161u+(index%588u)/28u,out,count)&&
            (!(index%28u)||nfc_emit(0x11a7u+index%28u,out,count));
    }
    const struct nfc_mapping *map=nfc_mapping(codepoint);
    if(!map)return nfc_emit(codepoint,out,count);
    return nfc_decompose(map->first,depth+1,out,count)&&
        (!map->second||nfc_decompose(map->second,depth+1,out,count));
}
static uint32_t nfc_compose(uint32_t first,uint32_t second){
    if(first>=0x1100u&&first<0x1100u+19u&&second>=0x1161u&&second<0x1161u+21u)
        return 0xac00u+((first-0x1100u)*21u+second-0x1161u)*28u;
    if(first>=0xac00u&&first<0xac00u+11172u&&!((first-0xac00u)%28u)&&
       second>0x11a7u&&second<0x11a7u+28u)return first+second-0x11a7u;
    size_t lo=0,hi=COUNT(nfc_composition_index);
    while(lo<hi){size_t middle=lo+(hi-lo)/2;const struct nfc_mapping *map=nfc_mappings+nfc_composition_index[middle];
        if(map->first<first||(map->first==first&&map->second<second))lo=middle+1;else hi=middle;}
    if(lo==COUNT(nfc_composition_index))return 0;
    const struct nfc_mapping *map=nfc_mappings+nfc_composition_index[lo];
    return map->first==first&&map->second==second?map->codepoint:0;
}
static int nfc_decode(const unsigned char *text,size_t *offset,uint32_t *out){
    size_t index=*offset;if(index>=NFC_NAME_BYTES)return 0;
    uint32_t first=text[index++],codepoint;unsigned need;uint32_t minimum;
    if(first<0x80u){if(!first)return 0;*out=first;*offset=index;return 1;}
    if(first>=0xc2u&&first<=0xdfu){codepoint=first&31u;need=1;minimum=0x80u;}
    else if(first>=0xe0u&&first<=0xefu){codepoint=first&15u;need=2;minimum=0x800u;}
    else if(first>=0xf0u&&first<=0xf4u){codepoint=first&7u;need=3;minimum=0x10000u;}
    else return 0;
    while(need--){if(index>=NFC_NAME_BYTES)return 0;unsigned byte=text[index++];
        if((byte&0xc0u)!=0x80u)return 0;
        codepoint=(codepoint<<6)|(byte&63u);}
    if(codepoint<minimum||codepoint>0x10ffffu||(codepoint>=0xd800u&&codepoint<=0xdfffu))return 0;
    *out=codepoint;*offset=index;return 1;
}
int lx06_unicode_nfc_name(void *context,const char *name){
    (void)context;if(!name)return 0;
    uint32_t original[NFC_NAME_SCALARS],normal[NFC_WORK_SCALARS];
    size_t offset=0,count=0,expanded=0;const unsigned char *text=(const unsigned char *)name;
    while(offset<=NFC_NAME_BYTES&&text[offset]){
        if(count==NFC_NAME_SCALARS||!nfc_decode(text,&offset,original+count)||nfc_no(original[count])||
           !nfc_decompose(original[count],0,normal,&expanded))return 0;
        count++;
    }
    if(!count||offset>NFC_NAME_BYTES||!expanded)return 0;
    size_t used=1,starter_position=0;unsigned last_class=nfc_ccc(normal[0]);
    uint32_t starter=last_class?0:normal[0];
    for(size_t i=1;i<expanded;i++){
        uint32_t codepoint=normal[i];unsigned current_class=nfc_ccc(codepoint);
        uint32_t composed=starter&&(last_class<current_class||!last_class)?nfc_compose(starter,codepoint):0;
        if(composed){normal[starter_position]=composed;starter=composed;}
        else{
            if(!current_class){starter=codepoint;starter_position=used;}
            normal[used++]=codepoint;last_class=current_class;
        }
    }
    if(used!=count)return 0;
    for(size_t i=0;i<count;i++)if(normal[i]!=original[i])return 0;
    return 1;
}
