/***************************************************************************
 *                                                                         *
 *   BNART.C                                                               *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   Building outgoing Usenet articles and reading the thread references  *
 *   and dates of incoming ones.                                           *
 *                                                                         *
 *   An outgoing article looks like this (CRLF line ends throughout):     *
 *                                                                         *
 *     From: Sysop <sysop@bbs.example.com>                                 *
 *     Newsgroups: alt.bbs                                                 *
 *     Subject: Re: Hello                                                  *
 *     Date: Mon, 20 Oct 2026 10:00:00 -0500                               *
 *     Message-ID: <...@bbs.example.com>                                   *
 *     References: <thread-start> <the-article-replied-to>                 *
 *     MIME-Version: 1.0                                                   *
 *     Content-Type: text/plain; charset=UTF-8                             *
 *     Content-Transfer-Encoding: 8bit                                     *
 *                                                                         *
 *     the text, converted from CP437 to UTF-8, colour codes removed       *
 *                                                                         *
 *   There is no Path: header: RFC 5537 leaves that to the news server.   *
 *                                                                         *
 *   No BBS SDK calls are made from this file.                             *
 *                                                                         *
 ***************************************************************************/

#include "BNART.H"
#include "SBMAIL.H"
#include "SBUTIL.H"
#include "IEMSG.H"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct obuf {              /* a growing output buffer              */
     CHAR *s;
     size_t len,cap;
     BOOL nomem;
} OBUF;

static VOID oput(OBUF *o, const CHAR *s, size_t n);
static VOID oputs(OBUF *o, const CHAR *s);
static VOID ohdr(OBUF *o, const CHAR *name, const CHAR *value);
static VOID obody(OBUF *o, const CHAR *cp437);
static BOOL isascii7(const CHAR *s);
static INT zoneoffset(const CHAR *z);

CHAR *                             /*   malloc'd article, NULL = no memory */
bnartBuild(                        /* build an outgoing article            */
const BNPOST *p,
const CHAR *body,
const CHAR *sig,
size_t *len)
{
     OBUF o;
     CHAR enc[512],tmp[640];
     CHAR refs[2*BN_MIDSZ+16];

     memset(&o,0,sizeof(o));

     /* From: "Display Name" <address>, or the bare address */
     if (p->fromname[0] != '\0') {
          if (isascii7(p->fromname)
           && strpbrk(p->fromname,"()<>@,;:\\\".[]") != NULL) {
               CHAR q[300];
               size_t i,n=0;

               q[n++]='"';
               for (i=0 ; p->fromname[i] != '\0' && n < sizeof(q)-3 ; i++) {
                    if (p->fromname[i] == '"' || p->fromname[i] == '\\') {
                         q[n++]='\\';
                    }
                    q[n++]=p->fromname[i];
               }
               q[n++]='"';
               q[n]='\0';
               sbStrCopy(enc,q,sizeof(enc));
          }
          else {
               bnartEncodeWord(p->fromname,enc,sizeof(enc));
          }
          _snprintf(tmp,sizeof(tmp)-1,"%s <%s>",enc,p->fromadr);
          tmp[sizeof(tmp)-1]='\0';
          ohdr(&o,"From",tmp);
     }
     else {
          ohdr(&o,"From",p->fromadr);
     }
     ohdr(&o,"Newsgroups",p->newsgroups);
     bnartEncodeWord(p->subject[0] != '\0' ? p->subject : "(no subject)",
                     enc,sizeof(enc));
     ohdr(&o,"Subject",enc);
     ohdr(&o,"Date",p->date);
     _snprintf(tmp,sizeof(tmp)-1,"<%s>",p->msgid);
     tmp[sizeof(tmp)-1]='\0';
     ohdr(&o,"Message-ID",tmp);

     /*
      * References: the thread's first article and the one replied to
      * (RFC 5537 allows a long chain to be trimmed this way).  Folded
      * onto two lines when both are present.
      */
     refs[0]='\0';
     if (p->parent[0] != '\0') {
          if (p->root[0] != '\0' && strcmp(p->root,p->parent) != 0) {
               _snprintf(refs,sizeof(refs)-1,"<%s>\r\n <%s>",p->root,
                         p->parent);
          }
          else {
               _snprintf(refs,sizeof(refs)-1,"<%s>",p->parent);
          }
          refs[sizeof(refs)-1]='\0';
          ohdr(&o,"References",refs);
     }
     if (p->org[0] != '\0') {
          bnartEncodeWord(p->org,enc,sizeof(enc));
          ohdr(&o,"Organization",enc);
     }
     if (p->useragent[0] != '\0') {
          ohdr(&o,"User-Agent",p->useragent);
     }
     ohdr(&o,"MIME-Version","1.0");
     ohdr(&o,"Content-Type","text/plain; charset=UTF-8");
     ohdr(&o,"Content-Transfer-Encoding","8bit");
     oputs(&o,"\r\n");

     obody(&o,body != NULL ? body : "");
     if (sig != NULL && sig[0] != '\0') {
          oputs(&o,"-- \r\n");     /* the standard signature separator    */
          obody(&o,sig);
     }
     if (o.nomem) {
          free(o.s);
          return(NULL);
     }
     *len=o.len;
     return(o.s);
}

VOID
bnartEncodeWord(                   /* header text, RFC 2047 if needed      */
const CHAR *cp437,
CHAR *dst,
size_t dstsz)
{
     CHAR utf8[600];
     size_t n,pos=0,o=0;

     sbCp437ToUtf8(cp437,utf8,sizeof(utf8));
     for (n=0 ; utf8[n] != '\0' ; n++) {
          if ((UCHAR)utf8[n] < 0x20) {
               utf8[n]=' ';        /* no control characters in headers    */
          }
     }
     if (isascii7(utf8)) {
          sbStrCopy(dst,utf8,dstsz);
          return;
     }
     /*
      * Base64 encoded words of at most 39 bytes of text each (so a word
      * stays well under the 75-character limit), never splitting a UTF-8
      * character, joined by folding white space.
      */
     dst[0]='\0';
     while (utf8[pos] != '\0') {
          CHAR b64[64];
          size_t take=strlen(utf8+pos);

          if (take > 39) {
               take=39;
               while (take > 0 && ((UCHAR)utf8[pos+take] & 0xC0) == 0x80) {
                    take--;        /* back up to a character boundary     */
               }
          }
          sbB64Encode(utf8+pos,take,b64);
          n=strlen(b64)+12+(o > 0 ? 3 : 0);
          if (o+n >= dstsz) {
               break;
          }
          o+=_snprintf(dst+o,dstsz-o-1,"%s=?UTF-8?B?%s?=",o > 0 ? "\r\n " : "",
                       b64);
          dst[o]='\0';
          pos+=take;
     }
}

INT                                /*   number of parents found            */
bnartRefs(                         /* thread references of an article      */
const CHAR *hdr,
size_t hlen,
CHAR *root,
CHAR (*parents)[BN_MIDSZ])
{
     CHAR *v;
     CHAR ids[64][BN_MIDSZ];
     INT n=0,i,np=0;
     const CHAR *p;

     root[0]='\0';
     if ((v=iemailHeader(hdr,hlen,"References",0)) == NULL
      && (v=iemailHeader(hdr,hlen,"In-Reply-To",0)) == NULL) {
          return(0);
     }
     /* every <id> in order; a very long chain keeps its first and last */
     for (p=v ; (p=strchr(p,'<')) != NULL ; ) {
          const CHAR *e=strchr(p,'>');
          size_t l;

          if (e == NULL) {
               break;
          }
          l=(size_t)(e-p-1);
          if (l > 0 && l < BN_MIDSZ && memchr(p+1,' ',l) == NULL) {
               if (n == 64) {
                    memmove(ids[1],ids[2],62*BN_MIDSZ);
                    n=63;
               }
               memcpy(ids[n],p+1,l);
               ids[n][l]='\0';
               n++;
          }
          p=e+1;
     }
     free(v);
     if (n == 0) {
          return(0);
     }
     sbStrCopy(root,ids[0],BN_MIDSZ);
     for (i=n-1 ; i >= 0 && np < BN_MAXPAR ; i--) {
          sbStrCopy(parents[np++],ids[i],BN_MIDSZ);
     }
     return(np);
}

BOOL                               /*   TRUE if the date could be read     */
bnartParseDate(                    /* RFC 5322 date to a UTC time          */
const CHAR *date,
time_t *utc)
{
     static const CHAR *mons="janfebmaraprmayjunjulaugsepoctnovdec";
     CHAR mon[8],zone[16];
     INT day,year,hh,mm,ss=0,used=0,off;
     const CHAR *p=date;
     const CHAR *m;
     struct tm t;
     time_t v;

     while (*p == ' ' || *p == '\t') {
          p++;
     }
     /* optional day of the week: "Mon," */
     if (isalpha((UCHAR)*p)) {
          while (isalpha((UCHAR)*p)) {
               p++;
          }
          while (*p == ',' || *p == ' ') {
               p++;
          }
     }
     zone[0]='\0';
     if (sscanf(p,"%d %3s %d %d:%d%n",&day,mon,&year,&hh,&mm,&used) != 5) {
          return(FALSE);
     }
     p+=used;
     if (*p == ':') {
          if (sscanf(p,":%d%n",&ss,&used) != 1) {
               return(FALSE);
          }
          p+=used;
     }
     sscanf(p," %15s",zone);
     mon[3]='\0';
     _strlwr(mon);
     if (strlen(mon) != 3 || (m=strstr(mons,mon)) == NULL || (m-mons)%3 != 0) {
          return(FALSE);
     }
     if (year < 50) {              /* obsolete 2-digit years (RFC 5322)   */
          year+=2000;
     }
     else if (year < 1000) {
          year+=1900;
     }
     if (day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60
      || year < 1970 || year > 3000) {
          return(FALSE);
     }
     memset(&t,0,sizeof(t));
     t.tm_year=year-1900;
     t.tm_mon=(INT)((m-mons)/3);
     t.tm_mday=day;
     t.tm_hour=hh;
     t.tm_min=mm;
     t.tm_sec=(ss == 60) ? 59 : ss;
     if ((v=_mkgmtime(&t)) == (time_t)-1) {
          return(FALSE);
     }
     off=zoneoffset(zone);         /* minutes east of UTC                 */
     *utc=v-(time_t)off*60;
     return(TRUE);
}

VOID
bnartMakeMsgId(                    /* a new Message-ID (without < >)       */
CHAR *dst,
size_t dstsz,
ULONG a,
ULONG b,
const CHAR *host)
{
     /*
      * The GME message id makes it unique on this BBS; the time makes it
      * unique even if the BBS's message base is ever started over.
      */
     _snprintf(dst,dstsz-1,"%lX.%lX.%llX.snbetnws@%s",a,b,
               (unsigned long long)time(NULL),
               (host != NULL && host[0] != '\0') ? host : "bbs.invalid");
     dst[dstsz-1]='\0';
}

/*--- helpers ------------------------------------------------------------*/

static INT                         /*   minutes east of UTC                */
zoneoffset(
const CHAR *z)
{
     static const struct { const CHAR *name; INT off; } names[]={
          {"UT",0},{"UTC",0},{"GMT",0},{"Z",0},
          {"EST",-300},{"EDT",-240},{"CST",-360},{"CDT",-300},
          {"MST",-420},{"MDT",-360},{"PST",-480},{"PDT",-420}
     };
     INT i;

     if ((z[0] == '+' || z[0] == '-') && isdigit((UCHAR)z[1])
      && isdigit((UCHAR)z[2]) && isdigit((UCHAR)z[3])
      && isdigit((UCHAR)z[4])) {
          INT v=((z[1]-'0')*10+(z[2]-'0'))*60+(z[3]-'0')*10+(z[4]-'0');
          return(z[0] == '-' ? -v : v);
     }
     for (i=0 ; i < (INT)(sizeof(names)/sizeof(names[0])) ; i++) {
          if (_stricmp(z,names[i].name) == 0) {
               return(names[i].off);
          }
     }
     return(0);                    /* unknown zone: treat as UTC          */
}

static BOOL
isascii7(
const CHAR *s)
{
     for ( ; *s != '\0' ; s++) {
          if ((UCHAR)*s >= 0x80) {
               return(FALSE);
          }
     }
     return(TRUE);
}

static VOID
oput(
OBUF *o,
const CHAR *s,
size_t n)
{
     if (o->nomem) {
          return;
     }
     if (o->len+n+1 > o->cap) {
          size_t want=(o->cap == 0) ? 4096 : o->cap*2;
          CHAR *bigger;

          while (want < o->len+n+1) {
               want*=2;
          }
          if ((bigger=(CHAR *)realloc(o->s,want)) == NULL) {
               o->nomem=TRUE;
               return;
          }
          o->s=bigger;
          o->cap=want;
     }
     memcpy(o->s+o->len,s,n);
     o->len+=n;
     o->s[o->len]='\0';
}

static VOID
oputs(
OBUF *o,
const CHAR *s)
{
     oput(o,s,strlen(s));
}

static VOID
ohdr(                              /* "Name: value" CRLF                   */
OBUF *o,
const CHAR *name,
const CHAR *value)
{
     oputs(o,name);
     oputs(o,": ");
     oputs(o,value);
     oputs(o,"\r\n");
}

/*
 * BBS text to article text: colour codes removed and CRLF line ends
 * (sbCleanBbsText), CP437 to UTF-8, and no line longer than the 998
 * bytes an article line may hold (longer ones are broken, never in the
 * middle of a character).
 */
static VOID
obody(
OBUF *o,
const CHAR *cp437)
{
     size_t n=strlen(cp437);
     CHAR *clean,*utf8;
     const CHAR *line;

     clean=(CHAR *)malloc(n*2+3);
     utf8=(CHAR *)malloc((n*2+3)*3);
     if (clean == NULL || utf8 == NULL) {
          free(clean);
          free(utf8);
          o->nomem=TRUE;
          return;
     }
     sbCleanBbsText(cp437,clean,n*2+3);
     sbCp437ToUtf8(clean,utf8,(n*2+3)*3);
     for (line=utf8 ; *line != '\0' ; ) {
          const CHAR *eol=strstr(line,"\r\n");
          size_t ll=(eol != NULL) ? (size_t)(eol-line) : strlen(line);

          while (ll > BN_LINEMAX) {
               size_t cut=BN_LINEMAX;

               while (cut > 0 && ((UCHAR)line[cut] & 0xC0) == 0x80) {
                    cut--;
               }
               oput(o,line,cut);
               oputs(o,"\r\n");
               line+=cut;
               ll-=cut;
          }
          oput(o,line,ll);
          oputs(o,"\r\n");
          line+=ll;
          if (*line == '\r') {
               line+=2;
          }
     }
     free(clean);
     free(utf8);
}
