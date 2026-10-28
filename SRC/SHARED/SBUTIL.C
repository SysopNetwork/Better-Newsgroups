/***************************************************************************
 *                                                                         *
 *   SBUTIL.C                                                              *
 *                                                                         *
 *   Better SMTP Relay (SNBSMTPR) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   Base64, HMAC-MD5, bounded string helpers, and a minimal ZIP writer.   *
 *                                                                         *
 ***************************************************************************/

#include "SBUTIL.H"
#include <bcrypt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "bcrypt.lib")

static const CHAR b64chars[]=
     "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t                             /*   chars needed incl. terminating NUL */
sbB64Size(                         /* room needed to Base64-encode n bytes */
size_t n)                          /*   number of input bytes              */
{
     return(((n+2)/3)*4+1);
}

size_t                             /*   length of the encoded text         */
sbB64Encode(                       /* Base64-encode a block (no line breaks)*/
const VOID *src,                   /*   bytes to encode                    */
size_t n,                          /*   number of bytes                    */
CHAR *dst)                         /*   output, at least sbB64Size(n) chars*/
{
     const UCHAR *s=(const UCHAR *)src;
     CHAR *d=dst;
     size_t i;

     /* every 3 input bytes become 4 output characters */
     for (i=0 ; i+2 < n ; i+=3) {
          *d++=b64chars[s[i]>>2];
          *d++=b64chars[((s[i]&0x03)<<4)|(s[i+1]>>4)];
          *d++=b64chars[((s[i+1]&0x0F)<<2)|(s[i+2]>>6)];
          *d++=b64chars[s[i+2]&0x3F];
     }
     /* 1 or 2 bytes left over are padded with '=' */
     if (i < n) {
          *d++=b64chars[s[i]>>2];
          if (i+1 < n) {
               *d++=b64chars[((s[i]&0x03)<<4)|(s[i+1]>>4)];
               *d++=b64chars[(s[i+1]&0x0F)<<2];
          }
          else {
               *d++=b64chars[(s[i]&0x03)<<4];
               *d++='=';
          }
          *d++='=';
     }
     *d='\0';
     return((size_t)(d-dst));
}

static INT
b64val(                            /* value of one Base64 character        */
CHAR c)
{
     if (c >= 'A' && c <= 'Z') {
          return(c-'A');
     }
     if (c >= 'a' && c <= 'z') {
          return(c-'a'+26);
     }
     if (c >= '0' && c <= '9') {
          return(c-'0'+52);
     }
     if (c == '+') {
          return(62);
     }
     if (c == '/') {
          return(63);
     }
     return(-1);
}

INT                                /*   decoded length, or -1 if invalid   */
sbB64Decode(                       /* decode Base64 text                   */
const CHAR *src,                   /*   Base64 text (NUL-terminated)       */
VOID *dst,                         /*   output buffer                      */
INT max)                           /*   size of the output buffer          */
{
     UCHAR *d=(UCHAR *)dst;
     INT len=0,bits=0;
     ULONG acc=0;

     for ( ; *src != '\0' && *src != '=' ; src++) {
          INT v;

          if (*src == ' ' || *src == '\r' || *src == '\n' || *src == '\t') {
               continue;
          }
          if ((v=b64val(*src)) < 0) {
               return(-1);
          }
          acc=(acc<<6)|(ULONG)v;
          bits+=6;
          if (bits >= 8) {
               bits-=8;
               if (len >= max) {
                    return(-1);
               }
               d[len++]=(UCHAR)((acc>>bits)&0xFF);
          }
     }
     return(len);
}

BOOL                               /*   TRUE on success                    */
sbHmacMd5Hex(                      /* HMAC-MD5 as 32 lowercase hex digits  */
const VOID *key,                   /*   key bytes                          */
INT keylen,                        /*   key length                         */
const VOID *msg,                   /*   message bytes                      */
INT msglen,                        /*   message length                     */
CHAR *hex)                         /*   output, at least 33 chars          */
{
     BCRYPT_ALG_HANDLE alg=NULL;
     BCRYPT_HASH_HANDLE hash=NULL;
     UCHAR digest[16];
     BOOL ok=FALSE;
     INT i;

     /* Windows' CNG library does the actual cryptography */
     if (BCryptOpenAlgorithmProvider(&alg,BCRYPT_MD5_ALGORITHM,NULL,
                                     BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
          return(FALSE);
     }
     if (BCryptCreateHash(alg,&hash,NULL,0,(PUCHAR)key,(ULONG)keylen,0) == 0
      && BCryptHashData(hash,(PUCHAR)msg,(ULONG)msglen,0) == 0
      && BCryptFinishHash(hash,digest,sizeof(digest),0) == 0) {
          for (i=0 ; i < 16 ; i++) {
               sprintf(hex+i*2,"%02x",digest[i]);
          }
          ok=TRUE;
     }
     if (hash != NULL) {
          BCryptDestroyHash(hash);
     }
     BCryptCloseAlgorithmProvider(alg,0);
     return(ok);
}

CHAR *                             /*   returns dst                        */
sbStrCopy(                         /* bounded copy, always NUL-terminated  */
CHAR *dst,                         /*   destination                        */
const CHAR *src,                   /*   source (NULL is treated as "")     */
size_t dstsz)                      /*   size of destination                */
{
     size_t len;

     if (dstsz == 0) {
          return(dst);
     }
     if (src == NULL) {
          src="";
     }
     len=strlen(src);
     if (len >= dstsz) {
          len=dstsz-1;
     }
     memmove(dst,src,len);
     dst[len]='\0';
     return(dst);
}

CHAR *                             /*   returns dst                        */
sbStrCat(                          /* bounded append, always NUL-terminated*/
CHAR *dst,                         /*   destination                        */
const CHAR *src,                   /*   text to append                     */
size_t dstsz)                      /*   size of destination                */
{
     size_t cur=strlen(dst);

     if (cur < dstsz) {
          sbStrCopy(dst+cur,src,dstsz-cur);
     }
     return(dst);
}

ULONG                              /*   the CRC                            */
sbCrc32(                           /* CRC-32 as used by ZIP (IEEE 802.3)   */
const VOID *data,                  /*   bytes                              */
size_t n)                          /*   number of bytes                    */
{
     const UCHAR *p=(const UCHAR *)data;
     ULONG crc=0xFFFFFFFFUL;
     size_t i;
     INT k;

     for (i=0 ; i < n ; i++) {
          crc^=p[i];
          for (k=0 ; k < 8 ; k++) {
               crc=(crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
          }
     }
     return(crc ^ 0xFFFFFFFFUL);
}

static UCHAR *
put16(                             /* little-endian 16-bit value           */
UCHAR *p,
ULONG v)
{
     p[0]=(UCHAR)(v & 0xFF);
     p[1]=(UCHAR)((v >> 8) & 0xFF);
     return(p+2);
}

static UCHAR *
put32(                             /* little-endian 32-bit value           */
UCHAR *p,
ULONG v)
{
     p=put16(p,v & 0xFFFF);
     return(put16(p,(v >> 16) & 0xFFFF));
}

/*
 * One file, method 0 (stored): local header, data, central directory and
 * end record.  No compression is needed; the point is only to give a
 * file a container its recipient can open.  The name keeps the BBS's
 * CP437 (flag bit 11, "UTF-8 name", stays clear).
 */
CHAR *                             /*   malloc'd ZIP file, NULL if no memory*/
sbZipStore(                        /* a one-file ZIP, stored uncompressed  */
const CHAR *name,                  /*   file name inside the ZIP (CP437)   */
const VOID *data,                  /*   file contents                      */
size_t n,                          /*   file length                        */
time_t when,                       /*   file date and time                 */
size_t *zlen)                      /*   receives the ZIP's length          */
{
     size_t nlen=strlen(name),total=30+nlen+n+46+nlen+22;
     ULONG crc=sbCrc32(data,n),dtime=0,ddate=(1 << 5) | 1;  /* 1980-01-01 */
     struct tm *tm=localtime(&when);
     UCHAR *z,*p,*cd;

     if (tm != NULL && tm->tm_year >= 80) {
          dtime=((ULONG)tm->tm_hour << 11) | ((ULONG)tm->tm_min << 5)
               | ((ULONG)tm->tm_sec/2);
          ddate=((ULONG)(tm->tm_year-80) << 9) | ((ULONG)(tm->tm_mon+1) << 5)
               | (ULONG)tm->tm_mday;
     }
     if ((z=(UCHAR *)malloc(total)) == NULL) {
          return(NULL);
     }
     p=z;
     p=put32(p,0x04034B50UL);      /* local file header                    */
     p=put16(p,10);                /*   version needed: 1.0                */
     p=put16(p,0);                 /*   flags                              */
     p=put16(p,0);                 /*   method: stored                     */
     p=put16(p,dtime);
     p=put16(p,ddate);
     p=put32(p,crc);
     p=put32(p,(ULONG)n);          /*   compressed size                    */
     p=put32(p,(ULONG)n);          /*   uncompressed size                  */
     p=put16(p,(ULONG)nlen);
     p=put16(p,0);                 /*   extra field length                 */
     memcpy(p,name,nlen);
     p+=nlen;
     memcpy(p,data,n);
     p+=n;
     cd=p;
     p=put32(p,0x02014B50UL);      /* central directory entry              */
     p=put16(p,20);                /*   made by: MS-DOS, 2.0               */
     p=put16(p,10);                /*   version needed                     */
     p=put16(p,0);                 /*   flags                              */
     p=put16(p,0);                 /*   method                             */
     p=put16(p,dtime);
     p=put16(p,ddate);
     p=put32(p,crc);
     p=put32(p,(ULONG)n);
     p=put32(p,(ULONG)n);
     p=put16(p,(ULONG)nlen);
     p=put16(p,0);                 /*   extra field length                 */
     p=put16(p,0);                 /*   comment length                     */
     p=put16(p,0);                 /*   disk number                        */
     p=put16(p,0);                 /*   internal attributes                */
     p=put32(p,0x20);              /*   external attributes: archive       */
     p=put32(p,0);                 /*   local header offset                */
     memcpy(p,name,nlen);
     p+=nlen;
     p=put32(p,0x06054B50UL);      /* end of central directory             */
     p=put16(p,0);                 /*   this disk                          */
     p=put16(p,0);                 /*   disk with the directory            */
     p=put16(p,1);                 /*   entries on this disk               */
     p=put16(p,1);                 /*   entries in all                     */
     p=put32(p,(ULONG)(46+nlen));  /*   directory size                     */
     p=put32(p,(ULONG)(cd-z));     /*   directory offset                   */
     p=put16(p,0);                 /*   comment length                     */
     *zlen=(size_t)(p-z);
     return((CHAR *)z);
}

/*
 * Several files in one stored (uncompressed) ZIP, laid out like
 * sbZipStore(): every local header and its data, then the central
 * directory, then the end record.
 */
CHAR *                             /*   malloc'd ZIP file, NULL if no memory*/
sbZipStoreMany(                    /* a ZIP of several files, uncompressed */
INT nfiles,                        /*   how many files (1-65535)           */
const CHAR * const *names,         /*   names inside the ZIP (CP437)       */
const VOID * const *datas,         /*   each file's contents               */
const size_t *lens,                /*   each file's length                 */
time_t when,                       /*   date and time for all files        */
size_t *zlen)                      /*   receives the ZIP's length          */
{
     size_t total=22,dirsize=0;
     ULONG dtime=0,ddate=(1 << 5) | 1;  /* 1980-01-01 */
     struct tm *tm=localtime(&when);
     UCHAR *z,*p,*cd;
     ULONG *crcs,*offs;
     INT i;

     if (nfiles < 1 || nfiles > 65535) {
          return(NULL);
     }
     for (i=0 ; i < nfiles ; i++) {
          size_t nlen=strlen(names[i]);
          total+=30+nlen+lens[i]+46+nlen;
          dirsize+=46+nlen;
     }
     if (tm != NULL && tm->tm_year >= 80) {
          dtime=((ULONG)tm->tm_hour << 11) | ((ULONG)tm->tm_min << 5)
               | ((ULONG)tm->tm_sec/2);
          ddate=((ULONG)(tm->tm_year-80) << 9) | ((ULONG)(tm->tm_mon+1) << 5)
               | (ULONG)tm->tm_mday;
     }
     z=(UCHAR *)malloc(total);
     crcs=(ULONG *)malloc(nfiles*sizeof(ULONG));
     offs=(ULONG *)malloc(nfiles*sizeof(ULONG));
     if (z == NULL || crcs == NULL || offs == NULL) {
          free(z);
          free(crcs);
          free(offs);
          return(NULL);
     }
     p=z;
     for (i=0 ; i < nfiles ; i++) {
          size_t nlen=strlen(names[i]);
          crcs[i]=sbCrc32(datas[i],lens[i]);
          offs[i]=(ULONG)(p-z);
          p=put32(p,0x04034B50UL); /* local file header                    */
          p=put16(p,10);           /*   version needed: 1.0                */
          p=put16(p,0);            /*   flags                              */
          p=put16(p,0);            /*   method: stored                     */
          p=put16(p,dtime);
          p=put16(p,ddate);
          p=put32(p,crcs[i]);
          p=put32(p,(ULONG)lens[i]);
          p=put32(p,(ULONG)lens[i]);
          p=put16(p,(ULONG)nlen);
          p=put16(p,0);            /*   extra field length                 */
          memcpy(p,names[i],nlen);
          p+=nlen;
          memcpy(p,datas[i],lens[i]);
          p+=lens[i];
     }
     cd=p;
     for (i=0 ; i < nfiles ; i++) {
          size_t nlen=strlen(names[i]);
          p=put32(p,0x02014B50UL); /* central directory entry              */
          p=put16(p,20);           /*   made by: MS-DOS, 2.0               */
          p=put16(p,10);           /*   version needed                     */
          p=put16(p,0);            /*   flags                              */
          p=put16(p,0);            /*   method                             */
          p=put16(p,dtime);
          p=put16(p,ddate);
          p=put32(p,crcs[i]);
          p=put32(p,(ULONG)lens[i]);
          p=put32(p,(ULONG)lens[i]);
          p=put16(p,(ULONG)nlen);
          p=put16(p,0);            /*   extra field length                 */
          p=put16(p,0);            /*   comment length                     */
          p=put16(p,0);            /*   disk number                        */
          p=put16(p,0);            /*   internal attributes                */
          p=put32(p,0x20);         /*   external attributes: archive       */
          p=put32(p,offs[i]);      /*   local header offset                */
          memcpy(p,names[i],nlen);
          p+=nlen;
     }
     p=put32(p,0x06054B50UL);      /* end of central directory             */
     p=put16(p,0);                 /*   this disk                          */
     p=put16(p,0);                 /*   disk with the directory            */
     p=put16(p,(ULONG)nfiles);     /*   entries on this disk               */
     p=put16(p,(ULONG)nfiles);     /*   entries in all                     */
     p=put32(p,(ULONG)dirsize);    /*   directory size                     */
     p=put32(p,(ULONG)(cd-z));     /*   directory offset                   */
     p=put16(p,0);                 /*   comment length                     */
     free(crcs);
     free(offs);
     *zlen=(size_t)(p-z);
     return((CHAR *)z);
}
