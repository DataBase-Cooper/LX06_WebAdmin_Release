/* Generic, bounded request envelope: AES-128-CBC + RSA-OAEP-SHA256(key||IV).
 * All platform URLs/public keys/policies stay in the upload script. No secrets
 * in argv; plaintext arrives on stdin. Legacy build uses the device crypto ABI.
 * Invoked only on login actions, with a private TMP lifetime, never a daemon. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "hash23.h"
#ifdef LX06_OPENSSL_1_0_COMPAT
typedef struct rsa_st RSA;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_md_st EVP_MD;
extern RSA *d2i_RSA_PUBKEY(RSA **,const unsigned char **,long);
extern int RSA_size(const RSA *);
extern void RSA_free(RSA *);
extern int RAND_bytes(unsigned char *,int);
extern const EVP_MD *EVP_sha256(void);
extern int RSA_padding_add_PKCS1_OAEP_mgf1(unsigned char *,int,const unsigned char *,int,const unsigned char *,int,const EVP_MD *,const EVP_MD *);
extern int RSA_public_encrypt(int,const unsigned char *,unsigned char *,RSA *,int);
extern EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
extern void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *);
extern const EVP_CIPHER *EVP_aes_128_cbc(void);
extern int EVP_EncryptInit_ex(EVP_CIPHER_CTX *,const EVP_CIPHER *,void *,const unsigned char *,const unsigned char *);
extern int EVP_EncryptUpdate(EVP_CIPHER_CTX *,unsigned char *,int *,const unsigned char *,int);
extern int EVP_EncryptFinal_ex(EVP_CIPHER_CTX *,unsigned char *,int *);
extern int EVP_EncodeBlock(unsigned char *,const unsigned char *,int);
extern void OPENSSL_cleanse(void *,size_t);
#define RSA_NO_PADDING 3
#else
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#endif

static int output(const char *path,const void *buf,size_t length) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return 0;
    const unsigned char *p=buf;size_t done=0;
    while(done<length){ssize_t n=write(fd,p+done,length-done);if(n<=0){close(fd);return 0;}done+=(size_t)n;}
    return close(fd)==0;
}
static int read_small(const char *path,char *data,size_t cap) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return 0;
    ssize_t n=read(fd,data,cap-1);char extra;int overflow=read(fd,&extra,1)>0;close(fd);
    if(n<=0||overflow)return 0;
    data[n]=0;return 1;
}
static int solve_files(char **argv) {
    char salt[129],expiry[20],limit[12],hex[66];unsigned char target[32];
    if(!read_small(argv[2],salt,sizeof(salt))||!read_small(argv[3],expiry,sizeof(expiry))||
       !read_small(argv[4],limit,sizeof(limit))||!read_small(argv[5],hex,sizeof(hex)))return 3;
    for(char *p=salt;*p;p++)if(!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-",*p))return 3;
    for(char *p=expiry;*p;p++)if(*p<'0'||*p>'9')return 3;
    for(char *p=limit;*p;p++)if(*p<'0'||*p>'9')return 3;
    if(strlen(hex)!=64)return 3;
    for(int i=0;i<32;i++) {
        const char *digits="0123456789abcdef";const char *a=strchr(digits,hex[2*i]),*b=strchr(digits,hex[2*i+1]);
        if(!a||!b)return 3;
        target[i]=(unsigned char)(((a-digits)<<4)|(b-digits));
    }
    unsigned max=0;
    for(char *p=limit;*p;p++) {
        max=max*10+(unsigned)(*p-'0');
        if(max>200000)return 3;
    }
    if(!max)return 3;
    int answer=hash23_solve(salt,expiry,(unsigned)max,target);
    if(answer<0)return 4;
    printf("%d",answer);return 0;
}
int main(int argc,char **argv) {
    if(argc==6&&!strcmp(argv[1],"--hash23-solve"))return solve_files(argv);
    if(argc==2&&!strcmp(argv[1],"--base64")) {
        unsigned char data[4096],encoded[5465];size_t n=fread(data,1,sizeof(data),stdin);
        if(n==sizeof(data)||ferror(stdin))return 3;
        int len=EVP_EncodeBlock(encoded,data,(int)n);fwrite(encoded,1,(size_t)len,stdout);return 0;
    }
    if(argc!=4)return 2;
    unsigned char der[2048],plain[4096],cipher[4112],material[32],padded[512],wrapped[512],header[769];
    int rc=3,n1=0,n2=0,keylen;RSA *rsa=NULL;EVP_CIPHER_CTX *ctx=NULL;
    int fd=open(argv[1],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)goto done;
    ssize_t size=read(fd,der,sizeof(der));close(fd);
    if(size<=0||size==(ssize_t)sizeof(der))goto done;
    const unsigned char *p=der;rsa=d2i_RSA_PUBKEY(NULL,&p,(long)size);
    if(!rsa||p!=der+size)goto done;
    keylen=RSA_size(rsa);if(keylen<256||keylen>512)goto done;
    size_t count=fread(plain,1,sizeof(plain),stdin);
    if(!count||count==sizeof(plain)||ferror(stdin))goto done;
    if(RAND_bytes(material,sizeof(material))!=1)goto done;
    ctx=EVP_CIPHER_CTX_new();if(!ctx)goto done;
    if(EVP_EncryptInit_ex(ctx,EVP_aes_128_cbc(),NULL,material,material+16)!=1||
       EVP_EncryptUpdate(ctx,cipher,&n1,plain,(int)count)!=1||
       EVP_EncryptFinal_ex(ctx,cipher+n1,&n2)!=1)goto done;
    if(RSA_padding_add_PKCS1_OAEP_mgf1(padded,keylen,material,sizeof(material),NULL,0,EVP_sha256(),EVP_sha256())!=1)goto done;
    if(RSA_public_encrypt(keylen,padded,wrapped,rsa,RSA_NO_PADDING)!=keylen)goto done;
    int length=EVP_EncodeBlock(header,wrapped,keylen);
    if(length<=0||length>=(int)sizeof(header))goto done;
    if(!output(argv[2],cipher,(size_t)(n1+n2))||!output(argv[3],header,(size_t)length))goto done;
    rc=0;
done:
    if(ctx)EVP_CIPHER_CTX_free(ctx);
    if(rsa)RSA_free(rsa);
    OPENSSL_cleanse(plain,sizeof(plain));OPENSSL_cleanse(material,sizeof(material));
    OPENSSL_cleanse(padded,sizeof(padded));
    return rc;
}
