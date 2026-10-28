/***************************************************************************
 *                                                                         *
 *   SBMAIL.C                                                              *
 *                                                                         *
 *   Better SMTP Relay (SNBSMTPR) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   BBS-text-to-Internet-mail conversion and MIME message building.      *
 *                                                                         *
 *   The finished message always looks like this:                         *
 *     headers (From, To, Reply-To, Subject, Date, Message-ID, MIME)       *
 *     body as UTF-8 text (quoted-printable when it isn't plain ASCII)     *
 *     plus, when there is an attachment, a multipart/mixed wrapper with   *
 *     the file Base64-encoded as the second part                          *
 *                                                                         *
 *   No BBS SDK calls are made from this file.                             *
 *                                                                         *
 ***************************************************************************/

#include "SBMAIL.H"
#include "SBUTIL.H"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define MAXATT   (25L*1024L*1024L) /* largest attachment we will encode    */

/*
 * Unicode code points for CP437 characters 0x80-0xFF (accented letters,
 * box drawing, Greek and maths symbols), the character set BBS text uses.
 */
static const USHORT cp437hi[128]={
     0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,
     0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
     0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,
     0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
     0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,
     0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
     0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
     0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
     0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,
     0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
     0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,
     0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
     0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,
     0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
     0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,
     0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0
};

/* growable output buffer used while building the message */
typedef struct obuf {
     CHAR *p;
     size_t len,size;
     BOOL oom;                     /*   ran out of memory?                 */
} OBUF;

static VOID oput(OBUF *o, const CHAR *s, size_t n);
static VOID oputs(OBUF *o, const CHAR *s);
static VOID oprintf(OBUF *o, const CHAR *fmt, ...);
static BOOL isascii7(const CHAR *s, size_t n);
static BOOL longlines(const CHAR *s, size_t n);
static VOID putqp(OBUF *o, const CHAR *s, size_t n);
static VOID putb64lines(OBUF *o, const CHAR *s, size_t n);
static VOID putword(OBUF *o, const CHAR *s);
static VOID putaddr(OBUF *o, const CHAR *hdr, const CHAR *name,
                    const CHAR *adr);

size_t                             /*   bytes written (excluding NUL)      */
sbCp437ToUtf8(                     /* convert CP437 text to UTF-8          */
const CHAR *src,                   /*   CP437 text (NUL-terminated)        */
CHAR *dst,                         /*   output buffer                      */
size_t dstsz)                      /*   size of output (3x input is safe)  */
{
     const UCHAR *s=(const UCHAR *)src;
     size_t len=0;

     if (dstsz == 0) {
          return(0);
     }
     for ( ; *s != '\0' ; s++) {
          UINT u=(*s < 0x80) ? *s : cp437hi[*s-0x80];
          UCHAR enc[3];
          INT n,i;

          if (u < 0x80) {
               enc[0]=(UCHAR)u;
               n=1;
          }
          else if (u < 0x800) {
               enc[0]=(UCHAR)(0xC0|(u>>6));
               enc[1]=(UCHAR)(0x80|(u&0x3F));
               n=2;
          }
          else {
               enc[0]=(UCHAR)(0xE0|(u>>12));
               enc[1]=(UCHAR)(0x80|((u>>6)&0x3F));
               enc[2]=(UCHAR)(0x80|(u&0x3F));
               n=3;
          }
          if (len+n >= dstsz) {
               break;              /* never split a character             */
          }
          for (i=0 ; i < n ; i++) {
               dst[len++]=(CHAR)enc[i];
          }
     }
     dst[len]='\0';
     return(len);
}

size_t                             /*   bytes written (excluding NUL)      */
sbCleanBbsText(                    /* strip ANSI codes, normalize to CRLF  */
const CHAR *src,                   /*   BBS message text (CR or LF lines)  */
CHAR *dst,                         /*   output buffer                      */
size_t dstsz)                      /*   size (2x input + 3 is always safe) */
{
     const UCHAR *s=(const UCHAR *)src;
     size_t len=0;

     if (dstsz < 3) {
          if (dstsz > 0) {
               dst[0]='\0';
          }
          return(0);
     }
     while (*s != '\0' && len < dstsz-3) {
          if (*s == 0x1B) {
               /*
                * ANSI escape (colours, cursor moves): ESC '[' parameters,
                * ending in a letter.  Mail readers would show them as junk.
                */
               s++;
               if (*s == '[') {
                    s++;
                    while (*s != '\0' && !(*s >= 0x40 && *s <= 0x7E)) {
                         s++;
                    }
               }
               if (*s != '\0') {
                    s++;
               }
               continue;
          }
          if (*s == '\r' || *s == '\n') {
               /* CR, LF, CRLF or LFCR all become one CRLF */
               UCHAR first=*s++;
               if ((*s == '\r' || *s == '\n') && *s != first) {
                    s++;
               }
               dst[len++]='\r';
               dst[len++]='\n';
               continue;
          }
          if (*s < 0x20 && *s != '\t') {
               s++;                /* other control codes are dropped     */
               continue;
          }
          dst[len++]=(CHAR)*s++;
     }
     /* the body always ends with a complete line */
     if (len < 2 || dst[len-2] != '\r' || dst[len-1] != '\n') {
          dst[len++]='\r';
          dst[len++]='\n';
     }
     dst[len]='\0';
     return(len);
}

VOID
sbRfc5322Date(                     /* "Mon, 12 Oct 2026 10:00:00 -0500"    */
CHAR *dst,
size_t dstsz)
{
     static const CHAR *days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
     static const CHAR *mons[]={"Jan","Feb","Mar","Apr","May","Jun","Jul",
                                "Aug","Sep","Oct","Nov","Dec"};
     time_t now;
     struct tm lt,gt;
     LONG off;
     time_t loc,utc;

     /* local time plus its offset from UTC, as Windows reports them */
     time(&now);
     localtime_s(&lt,&now);
     gmtime_s(&gt,&now);
     gt.tm_isdst=lt.tm_isdst;
     loc=mktime(&lt);
     utc=mktime(&gt);
     off=(LONG)difftime(loc,utc)/60L;
     _snprintf(dst,dstsz-1,"%s, %02d %s %04d %02d:%02d:%02d %c%02ld%02ld",
               days[lt.tm_wday],lt.tm_mday,mons[lt.tm_mon],lt.tm_year+1900,
               lt.tm_hour,lt.tm_min,lt.tm_sec,off < 0 ? '-' : '+',
               labs(off)/60L,labs(off)%60L);
     dst[dstsz-1]='\0';
}

VOID
sbMakeMsgId(                       /* "<unique@host>"                      */
CHAR *dst,
size_t dstsz,
const CHAR *host)                  /*   our host name                      */
{
     static ULONG counter;
     FILETIME ft;
     ULONGLONG t;

     GetSystemTimeAsFileTime(&ft);
     t=((ULONGLONG)ft.dwHighDateTime<<32)|ft.dwLowDateTime;
     _snprintf(dst,dstsz-1,"<%llx.%lx.%lx.snbsmtpr@%s>",t,
               (unsigned long)GetCurrentProcessId(),
               (unsigned long)InterlockedIncrement((LONG *)&counter),
               host != NULL && host[0] != '\0' ? host : "localhost");
     dst[dstsz-1]='\0';
}

const CHAR *                       /*   e.g. "application/pdf"             */
sbMimeType(                        /* content type for a file name         */
const CHAR *fname)
{
     static const struct { const CHAR *ext,*type; } types[]={
          {"txt","text/plain"},{"asc","text/plain"},{"nfo","text/plain"},
          {"diz","text/plain"},{"htm","text/html"},{"html","text/html"},
          {"csv","text/csv"},{"jpg","image/jpeg"},{"jpeg","image/jpeg"},
          {"gif","image/gif"},{"png","image/png"},{"bmp","image/bmp"},
          {"pdf","application/pdf"},{"zip","application/zip"},
          {"gz","application/gzip"},{"7z","application/x-7z-compressed"},
          {"rar","application/vnd.rar"},{"arj","application/x-arj"},
          {"lzh","application/x-lzh"},{"doc","application/msword"},
          {"rtf","application/rtf"},{"mp3","audio/mpeg"},{"wav","audio/wav"}
     };
     const CHAR *dot=strrchr(fname,'.');
     INT i;

     if (dot != NULL) {
          for (i=0 ; i < (INT)(sizeof(types)/sizeof(types[0])) ; i++) {
               if (_stricmp(dot+1,types[i].ext) == 0) {
                    return(types[i].type);
               }
          }
     }
     return("application/octet-stream");
}

CHAR *                             /*   malloc'd file contents or NULL     */
sbReadFile(                        /* read a whole file into memory        */
const CHAR *path,
LONG *len,
LONG maxlen)
{
     FILE *fp;
     CHAR *buf;
     LONG size;

     *len=0;
     if ((fp=fopen(path,"rb")) == NULL) {
          return(NULL);
     }
     fseek(fp,0L,SEEK_END);
     size=ftell(fp);
     fseek(fp,0L,SEEK_SET);
     if (size < 0 || size > maxlen
      || (buf=(CHAR *)malloc((size_t)size+1)) == NULL) {
          fclose(fp);
          return(NULL);
     }
     if (fread(buf,1,(size_t)size,fp) != (size_t)size) {
          free(buf);
          fclose(fp);
          return(NULL);
     }
     fclose(fp);
     buf[size]='\0';
     *len=size;
     return(buf);
}

CHAR *                             /*   malloc'd UTF-8 text, NULL = failed */
sbmailReadBody(                    /* read the body file, as UTF-8         */
const SBMAIL *m,
LONG *len,
LONG maxlen)
{
     CHAR *raw,*utf8;
     LONG rawlen;
     size_t room;

     if ((raw=sbReadFile(m->bodyfile,&rawlen,maxlen)) == NULL) {
          return(NULL);
     }
     if (!m->cp437body) {
          *len=rawlen;
          return(raw);
     }
     /* each CP437 character becomes at most 3 UTF-8 bytes */
     room=(size_t)rawlen*3+1;
     if ((utf8=(CHAR *)malloc(room)) == NULL) {
          free(raw);
          return(NULL);
     }
     *len=(LONG)sbCp437ToUtf8(raw,utf8,room);
     free(raw);
     return(utf8);
}

CHAR *                             /*   malloc'd message, NULL on failure  */
sbmailBuildMime(                   /* full RFC 5322 / MIME message (CRLF)  */
const SBMAIL *m,
LONG *len,
CHAR *err,
size_t errsz)
{
     OBUF o;
     CHAR *body,*att=NULL,boundary[80];
     LONG bodylen,attlen=0;
     BOOL qp;

     memset(&o,0,sizeof(o));
     if ((body=sbmailReadBody(m,&bodylen,MAXATT)) == NULL) {
          sbStrCopy(err,"Could not read the queued message text",errsz);
          return(NULL);
     }
     if (m->attfile[0] != '\0') {
          if ((att=sbReadFile(m->attfile,&attlen,MAXATT)) == NULL) {
               sbStrCopy(err,"Could not read the attachment file",errsz);
               free(body);
               return(NULL);
          }
     }

     /* quoted-printable when the text is not 7-bit or has huge lines */
     qp=!isascii7(body,(size_t)bodylen) || longlines(body,(size_t)bodylen);

     /* headers */
     putaddr(&o,"From",m->fromname,m->fromadr);
     oprintf(&o,"To: <%s>\r\n",m->to);
     if (m->replyto[0] != '\0') {
          oprintf(&o,"Reply-To: <%s>\r\n",m->replyto);
     }
     oputs(&o,"Subject: ");
     putword(&o,m->subject[0] != '\0' ? m->subject : "(no subject)");
     oputs(&o,"\r\n");
     oprintf(&o,"Date: %s\r\n",m->date);
     oprintf(&o,"Message-ID: %s\r\n",m->msgid);
     oputs(&o,"MIME-Version: 1.0\r\n");

     if (att != NULL) {
          _snprintf(boundary,sizeof(boundary)-1,"=_snbsmtpr_%08lx%08lx",
                    (unsigned long)GetTickCount(),(unsigned long)attlen);
          boundary[sizeof(boundary)-1]='\0';
          oprintf(&o,"Content-Type: multipart/mixed; boundary=\"%s\"\r\n"
                     "\r\n"
                     "This is a multi-part message in MIME format.\r\n"
                     "\r\n"
                     "--%s\r\n",boundary,boundary);
     }

     /* the text part */
     oputs(&o,qp ? "Content-Type: text/plain; charset=UTF-8\r\n"
                   "Content-Transfer-Encoding: quoted-printable\r\n"
                 : "Content-Type: text/plain; charset=US-ASCII\r\n"
                   "Content-Transfer-Encoding: 7bit\r\n");
     oputs(&o,"\r\n");
     if (qp) {
          putqp(&o,body,(size_t)bodylen);
     }
     else {
          oput(&o,body,(size_t)bodylen);
     }

     /* the attachment part */
     if (att != NULL) {
          oprintf(&o,"\r\n--%s\r\n"
                     "Content-Type: %s; name=\"%s\"\r\n"
                     "Content-Transfer-Encoding: base64\r\n"
                     "Content-Disposition: attachment; filename=\"%s\"\r\n"
                     "\r\n",boundary,sbMimeType(m->attname),m->attname,
                     m->attname);
          putb64lines(&o,att,(size_t)attlen);
          oprintf(&o,"\r\n--%s--\r\n",boundary);
     }

     free(body);
     free(att);
     if (o.oom) {
          free(o.p);
          sbStrCopy(err,"Out of memory building the message",errsz);
          return(NULL);
     }
     *len=(LONG)o.len;
     return(o.p);
}

static VOID
putaddr(                           /* "From: "Name" <adr>" header          */
OBUF *o,
const CHAR *hdr,
const CHAR *name,
const CHAR *adr)
{
     oprintf(o,"%s: ",hdr);
     if (name != NULL && name[0] != '\0') {
          if (isascii7(name,strlen(name))) {
               const CHAR *p;

               /* a quoted string; quotes and backslashes get escaped */
               oputs(o,"\"");
               for (p=name ; *p != '\0' ; p++) {
                    if (*p == '"' || *p == '\\') {
                         oputs(o,"\\");
                    }
                    oput(o,p,1);
               }
               oputs(o,"\" ");
          }
          else {
               putword(o,name);
               oputs(o," ");
          }
     }
     oprintf(o,"<%s>\r\n",adr);
}

static VOID
putword(                           /* header text, RFC 2047 if not ASCII   */
OBUF *o,
const CHAR *s)
{
     size_t n=strlen(s);

     if (isascii7(s,n)) {
          oputs(o,s);
          return;
     }
     /*
      * "=?UTF-8?B?...?=" encoded words, each holding at most 45 bytes so
      * a word stays within the 75-character limit, and never splitting a
      * multi-byte UTF-8 character between two words.
      */
     while (n > 0) {
          size_t take=n > 45 ? 45 : n;
          CHAR b64[64];

          while (take < n && take > 0 && ((UCHAR)s[take]&0xC0) == 0x80) {
               take--;
          }
          sbB64Encode(s,take,b64);
          oprintf(o,"=?UTF-8?B?%s?=",b64);
          s+=take;
          n-=take;
          if (n > 0) {
               oputs(o,"\r\n ");   /* folded continuation line           */
          }
     }
}

static VOID
putqp(                             /* quoted-printable body (RFC 2045)     */
OBUF *o,
const CHAR *s,
size_t n)
{
     static const CHAR hex[]="0123456789ABCDEF";
     size_t i;
     INT col=0;

     for (i=0 ; i < n ; i++) {
          UCHAR c=(UCHAR)s[i];
          CHAR enc[4];
          INT elen;

          if (c == '\r' && i+1 < n && s[i+1] == '\n') {
               oputs(o,"\r\n");
               i++;
               col=0;
               continue;
          }
          /* spaces before a line break must be encoded to survive */
          if ((c == ' ' || c == '\t')
           && (i+1 >= n || s[i+1] == '\r')) {
               elen=3;
          }
          else if ((c >= 33 && c <= 126 && c != '=') || c == ' '
                || c == '\t') {
               elen=1;
          }
          else {
               elen=3;
          }
          if (elen == 1) {
               enc[0]=(CHAR)c;
          }
          else {
               enc[0]='=';
               enc[1]=hex[c>>4];
               enc[2]=hex[c&0x0F];
          }
          if (col+elen > 75) {
               oputs(o,"=\r\n");   /* soft line break                    */
               col=0;
          }
          oput(o,enc,(size_t)elen);
          col+=elen;
     }
}

static VOID
putb64lines(                       /* Base64 in 76-character lines         */
OBUF *o,
const CHAR *s,
size_t n)
{
     CHAR line[80];
     size_t i;

     for (i=0 ; i < n ; i+=57) {
          size_t take=(n-i > 57) ? 57 : n-i;
          sbB64Encode(s+i,take,line);
          oputs(o,line);
          oputs(o,"\r\n");
     }
}

static BOOL
isascii7(                          /* only 7-bit characters?               */
const CHAR *s,
size_t n)
{
     size_t i;

     for (i=0 ; i < n ; i++) {
          if ((UCHAR)s[i] >= 0x80) {
               return(FALSE);
          }
     }
     return(TRUE);
}

static BOOL
longlines(                         /* any line over SMTP's 998 limit?      */
const CHAR *s,
size_t n)
{
     size_t i,col=0;

     for (i=0 ; i < n ; i++) {
          if (s[i] == '\n') {
               col=0;
          }
          else if (++col > 998) {
               return(TRUE);
          }
     }
     return(FALSE);
}

static VOID
oput(                              /* append bytes to the output buffer    */
OBUF *o,
const CHAR *s,
size_t n)
{
     if (o->oom) {
          return;
     }
     if (o->len+n+1 > o->size) {
          size_t want=o->size == 0 ? 16384 : o->size;
          CHAR *bigger;

          while (want < o->len+n+1) {
               want*=2;
          }
          if ((bigger=(CHAR *)realloc(o->p,want)) == NULL) {
               o->oom=TRUE;
               return;
          }
          o->p=bigger;
          o->size=want;
     }
     memcpy(o->p+o->len,s,n);
     o->len+=n;
     o->p[o->len]='\0';
}

static VOID
oputs(
OBUF *o,
const CHAR *s)
{
     oput(o,s,strlen(s));
}

static VOID
oprintf(
OBUF *o,
const CHAR *fmt,
...)
{
     CHAR buf[1024];
     va_list ap;
     INT n;

     va_start(ap,fmt);
     n=_vsnprintf(buf,sizeof(buf)-1,fmt,ap);
     va_end(ap);
     buf[sizeof(buf)-1]='\0';
     if (n < 0) {
          n=(INT)strlen(buf);
     }
     oput(o,buf,(size_t)n);
}
