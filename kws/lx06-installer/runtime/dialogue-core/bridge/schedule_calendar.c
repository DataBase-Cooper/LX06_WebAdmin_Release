#include "schedule_calendar.h"
#include <stddef.h>
#define FIRST_DAY 10957
#define END_DAY 84006 /* 2200-01-01, exclusive */
#define DAY_MS INT64_C(86400000)
#define OFFSET_MS INT64_C(28800000)
static int leap(unsigned y){return !(y%4)&&(y%100||!(y%400));}
static unsigned days(unsigned y,unsigned m){
    static const unsigned n[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return n[m-1]+(m==2&&(unsigned)leap(y));
}
int br_calendar_day(unsigned y,unsigned m,unsigned d,int32_t *out){
    if(!out||y<2000||y>2199||!m||m>12||!d||d>days(y,m))return -1;
    int32_t n=FIRST_DAY;
    for(unsigned i=2000;i<y;i++)n+=365+leap(i);
    for(unsigned i=1;i<m;i++)n+=(int32_t)days(y,i);
    *out=n+(int32_t)d-1;return 1;
}
int br_calendar_date(int32_t n,unsigned *y,unsigned *m,unsigned *d){
    if(!y||!m||!d||n<FIRST_DAY||n>=END_DAY)return -1;
    unsigned year=2000,month=1; n-=FIRST_DAY;
    while(n>=365+leap(year)){n-=365+leap(year);year++;}
    while(n>=(int32_t)days(year,month)){n-=(int32_t)days(year,month);month++;}
    *y=year;*m=month;*d=(unsigned)n+1;return 1;
}
int br_calendar_valid(const br_calendar *c){
    if(!c||c->kind<BR_CAL_DAILY||c->kind>BR_CAL_MONTHLY||
       c->first_minute>c->last_minute||c->last_minute>1439)return 0;
    if(c->kind==BR_CAL_EVERY_N){
        if(c->anchor_day<FIRST_DAY||c->anchor_day>=END_DAY||!c->interval_days||c->interval_days>365)return 0;
    }else if(c->anchor_day||c->interval_days)return 0;
    if(c->kind==BR_CAL_WEEKLY){if(!c->weekdays||c->weekdays>127)return 0;}else if(c->weekdays)return 0;
    if(c->kind==BR_CAL_MONTHLY){if(!c->monthdays||(c->monthdays&UINT32_C(0x80000000)))return 0;}
    else if(c->monthdays)return 0;
    return 1;
}
int br_calendar_wall(int64_t wall,int32_t *day){
    if(!day||wall<(int64_t)FIRST_DAY*DAY_MS-OFFSET_MS||wall>=(int64_t)END_DAY*DAY_MS-OFFSET_MS)return -1;
    *day=(int32_t)((wall+OFFSET_MS)/DAY_MS);return 1;
}
int br_calendar_matches(const br_calendar *c,int32_t day){
    if(!br_calendar_valid(c)||day<FIRST_DAY||day>=END_DAY)return 0;
    if(c->kind==BR_CAL_DAILY)return 1;
    if(c->kind==BR_CAL_EVERY_N)return day>=c->anchor_day&&(day-c->anchor_day)%(int32_t)c->interval_days==0;
    if(c->kind==BR_CAL_WEEKLY)return !!(c->weekdays&(1u<<((day+3)%7)));
    unsigned y,m,d;
    if(br_calendar_date(day,&y,&m,&d)!=1)return 0;
    return !!(c->monthdays&(UINT32_C(1)<<(d-1)));
}
static uint64_t mix(uint64_t n){
    n=(n^(n>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    n=(n^(n>>27))*UINT64_C(0x94d049bb133111eb);return n^(n>>31);
}
int br_calendar_when(const br_calendar *c,int32_t day,uint64_t seed,int64_t *wall){
    if(!wall||!seed||!br_calendar_matches(c,day))return -1;
    uint64_t width=c->last_minute-c->first_minute+1;
    uint64_t threshold=(UINT64_C(0)-width)%width;
    uint64_t state=seed^((uint64_t)(uint32_t)day<<32),sample=0;
    unsigned i=0;
    do {state+=UINT64_C(0x9e3779b97f4a7c15);sample=mix(state);i++;}while(sample<threshold&&i<32);
    if(sample<threshold)return -1;
    unsigned minute=c->first_minute+(unsigned)(sample%width);
    *wall=(int64_t)day*DAY_MS-OFFSET_MS+(int64_t)minute*60000;return 1;
}
int br_calendar_find(const br_calendar *c,int64_t wall,uint64_t seed,int dir,int32_t *out,int64_t *when){
    int32_t day;
    if(!out||!when||!seed||!br_calendar_valid(c)||(dir!=1&&dir!=-1)||br_calendar_wall(wall,&day)!=1)return -1;
    /* Jump directly to a future EVERY_N anchor rather than scan decades. */
    if(c->kind==BR_CAL_EVERY_N&&day<c->anchor_day){if(dir<0)return 0;day=c->anchor_day;}
    for(unsigned i=0;i<367&&day>=FIRST_DAY&&day<END_DAY;i++,day+=dir){
        if(!br_calendar_matches(c,day))continue;
        int64_t stamp;if(br_calendar_when(c,day,seed,&stamp)!=1)return -1;
        if((dir>0&&stamp>=wall)||(dir<0&&stamp<=wall)){*out=day;*when=stamp;return 1;}
    }
    return 0;
}
