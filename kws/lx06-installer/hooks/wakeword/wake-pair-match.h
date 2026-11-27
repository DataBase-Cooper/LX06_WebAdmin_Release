/* Phase-only correlation of a stock scoring window against a continuous
 * physically qualified mono recording. All offsets are original 16 kHz
 * sample coordinates. No template, label or wall-clock similarity is used. */
#ifndef LX06_WAKE_PAIR_MATCH_H
#define LX06_WAKE_PAIR_MATCH_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct wake_complex { float r,i; };
struct wake_pair_result { int offset, next_offset, active, supporting; float peak, next; };
static void wake_fft(struct wake_complex *v,unsigned n,int inverse) {
    for(unsigned i=1,j=0;i<n;++i) {
        unsigned bit=n/2;
        while(j&bit) { j^=bit;bit/=2; }
        j^=bit;
        if(i<j) {struct wake_complex t=v[i];v[i]=v[j];v[j]=t;}
    }
    for(unsigned size=2;size<=n;size*=2) {
        double angle=(inverse ? 2. : -2.)*3.14159265358979323846/size;
        double dr=cos(angle),di=sin(angle),wr=1,wi=0;
        /* Keep a short contiguous batch of butterflies in cache. Generate
         * twiddles in the same order and precision as the scalar transform. */
        enum { TWIDDLES=32 };
        double real[TWIDDLES],imag[TWIDDLES];
        for(unsigned from=0;from<size/2;) {
            unsigned count=size/2-from;
            if(count>TWIDDLES)count=TWIDDLES;
            for(unsigned j=0;j<count;j++) {
                real[j]=wr;imag[j]=wi;
                double t=wr*dr-wi*di;wi=wr*di+wi*dr;wr=t;
            }
            for(unsigned base=from;base<n;base+=size) {
                for(unsigned j=0;j<count;j++) {
                    unsigned i=base+j;
                    struct wake_complex a=v[i],b=v[i+size/2];
                    float r=(float)(real[j]*b.r-imag[j]*b.i);
                    float im=(float)(real[j]*b.i+imag[j]*b.r);
                    v[i]=(struct wake_complex){a.r+r,a.i+im};
                    v[i+size/2]=(struct wake_complex){a.r-r,a.i-im};
                }
            }
            from+=count;
        }
    }
}
/* Pack two real signals into one FFT, then recover each spectrum from its
 * conjugate bin and retain only the normalized cross-spectrum. */
static float wake_cross_phase(struct wake_complex *v,unsigned n,unsigned lo,unsigned hi) {
    double sr=0,si=0;
    wake_fft(v,n,0);
    v[0]=v[n/2]=(struct wake_complex){0,0};
    for(unsigned k=1;k<n/2;k++) {
        struct wake_complex p=v[k],q=v[n-k],z={0,0};
        if(k>=lo && k<=hi) {
            double xr=p.r+q.r,xi=p.i-q.i,yr=p.i+q.i,yi=q.r-p.r;
            double r=yr*xr+yi*xi,i=yi*xr-yr*xi,mag=hypot(r,i);
            if(mag>1.e-6) {z.r=(float)(r/mag);z.i=(float)(i/mag);}
            sr+=z.r;si+=z.i;
        }
        v[k]=z;v[n-k]=(struct wake_complex){z.r,-z.i};
    }
    return (float)(hypot(sr,si)/(hi-lo+1));
}
static double wake_frame_energy(const int16_t *p) {
    double e=0;
    for(unsigned i=0;i<1024;i++) e+=(double)p[i]*p[i];
    return e;
}
static float wake_frame_phase(const int16_t *x,const int16_t *y,struct wake_complex *v) {
    for(unsigned i=0;i<1024;i++) {
        float w=(float)(.5-.5*cos(6.283185307179586*i/1023.));
        v[i]=(struct wake_complex){w*x[i],w*y[i]};
    }
    return wake_cross_phase(v,1024,16,239);
}
static int wake_pair_match(const int16_t *stock,int ns,const int16_t *physical,int np,
                           struct wake_pair_result *out) {
    memset(out,0,sizeof(*out));
    if(!stock || !physical || ns<9600 || ns>40000 || np<ns || np>80000) return 0;
    unsigned n=1;
    while(n<(unsigned)(ns+np)) n*=2;
    struct wake_complex *v=calloc(n,sizeof(*v));
    if(!v) return 0;
    for(int i=0;i<ns;i++) v[i].r=stock[i];
    for(int i=0;i<np;i++) v[i].i=physical[i];
    wake_cross_phase(v,n,(240*n+15999)/16000,3800*n/16000);
    wake_fft(v,n,1);
    for(int i=0;i<=np-ns;i++) {
        float score=fabsf(v[i].r)/n;
        if(score>out->peak) {out->offset=i;out->peak=score;}
    }
    /* A second distinct alignment, including a repeated utterance, must
     * fail closed. Nearby peaks within 15 ms can be room reflections. */
    for(int i=-ns/3;i<=np-ns+ns/3;i++) {
        if(abs(i-out->offset)<=240) continue;
        float score=fabsf(v[((unsigned)i)&(n-1)].r)/n;
        if(score>out->next) {out->next=score;out->next_offset=i;}
    }
    int valid=out->peak>=.10f && out->peak>3*out->next;
    if(valid) {
        double max_energy=0;
        for(int i=0;i+1024<=ns;i+=512) max_energy=fmax(max_energy,wake_frame_energy(stock+i));
        for(int i=0;i+1024<=ns;i+=512) {
            double e=wake_frame_energy(stock+i);
            if(e<1024.*100*100 || e<.05*max_energy) continue;
            ++out->active;
            if(wake_frame_phase(stock+i,physical+out->offset+i,v)>=.20f) ++out->supporting;
        }
        valid=out->active>=4 && out->supporting*2>out->active;
    }
    free(v);
    return valid;
}
#endif
