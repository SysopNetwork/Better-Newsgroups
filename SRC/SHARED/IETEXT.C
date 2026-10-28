/***************************************************************************
 *                                                                         *
 *   IETEXT.C                                                              *
 *                                                                         *
 *   Better Internet Email - mail receiver (SNBIEMLR) for The Major BBS v10*
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   Text conversions for incoming mail.  BBS users read mail on CP437     *
 *   terminals, so every piece of text is brought to CP437 the same way:  *
 *                                                                         *
 *     bytes in the sender's charset                                       *
 *       -> Unicode (Windows MultiByteToWideChar)                          *
 *       -> HTML turned into plain text, when the part is HTML             *
 *       -> typographic characters replaced (curly quotes, dashes, ...)    *
 *       -> control characters removed (no ANSI/escape codes can reach a   *
 *          user's terminal from an e-mail)                                *
 *       -> word-wrapped                                                   *
 *       -> CP437 (Windows WideCharToMultiByte, closest match per letter)  *
 *                                                                         *
 *   Worker-thread only: no BBS SDK calls are made from this file.         *
 *                                                                         *
 ***************************************************************************/

#include "IEMSG.H"
#include "SBUTIL.H"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct wbuf {              /* a growing Unicode (UTF-16) string    */
     WCHAR *s;
     size_t len;
     size_t cap;
     BOOL nomem;                   /*   an allocation failed               */
} WBUF;

static UINT codepage(const CHAR *charset);
static BOOL tounicode(WBUF *w, const CHAR *src, size_t len, UINT cp);
static VOID wput(WBUF *w, WCHAR c);
static VOID wputs(WBUF *w, const WCHAR *s);
static VOID html2text(const WCHAR *src, size_t len, WBUF *out);
static WCHAR entity(const WCHAR *s, size_t len, size_t *used);
static VOID tidy(const WCHAR *src, size_t len, WBUF *out, BOOL oneline);
static VOID wrap(const WCHAR *src, size_t len, INT col, WBUF *out);
static CHAR *tocp437(const WCHAR *src, size_t len, size_t *outlen);
static INT hexval(INT c);

CHAR *                             /*   malloc'd CP437 text, NULL = no mem */
ietextToCp437(                     /* convert text in any charset to CP437 */
const CHAR *src,                   /*   the text                           */
size_t len,                        /*   its length                         */
const CHAR *charset,               /*   MIME charset ("" = guess)          */
BOOL html,                         /*   convert HTML to plain text first   */
INT wrapcol,                       /*   word-wrap column (0 = no wrap)     */
size_t *outlen)                    /*   receives the length                */
{
     WBUF u,h,t,r;
     CHAR *out=NULL;

     memset(&u,0,sizeof(u));
     memset(&h,0,sizeof(h));
     memset(&t,0,sizeof(t));
     memset(&r,0,sizeof(r));
     *outlen=0;
     if (!tounicode(&u,src,len,codepage(charset))) {
          goto done;
     }
     if (html) {
          html2text(u.s,u.len,&h);
          if (h.nomem) {
               goto done;
          }
          tidy(h.s,h.len,&t,FALSE);
     }
     else {
          tidy(u.s,u.len,&t,FALSE);
     }
     if (t.nomem) {
          goto done;
     }
     if (wrapcol > 0) {
          wrap(t.s,t.len,wrapcol,&r);
          if (r.nomem) {
               goto done;
          }
          out=tocp437(r.s,r.len,outlen);
     }
     else {
          out=tocp437(t.s,t.len,outlen);
     }
done:
     free(u.s);
     free(h.s);
     free(t.s);
     free(r.s);
     return(out);
}

VOID
ietextDecodeWords(                 /* decode RFC 2047 =?cs?Q/B?...?= words */
const CHAR *src,                   /*   header text                        */
CHAR *dst,                         /*   receives one CP437 line            */
size_t dstsz)
{
     WBUF w,t;
     const CHAR *p=src,*plain=src;
     CHAR *word=NULL,*text;
     size_t outlen;
     BOOL lastenc=FALSE;

     memset(&w,0,sizeof(w));
     memset(&t,0,sizeof(t));
     dst[0]='\0';

     /*
      * Plain stretches are taken as UTF-8 when they are valid UTF-8 (some
      * mailers send raw UTF-8 headers), otherwise as Windows-1252.  The
      * gap between two encoded words is dropped, as RFC 2047 says.
      */
     while (*p != '\0') {
          const CHAR *q1=NULL,*q2=NULL,*q3=NULL,*end;

          /* an encoded word looks like  =?charset?Q?text?=  (or ?B?) */
          if (p[0] == '=' && p[1] == '?') {
               q1=strchr(p+2,'?');
               if (q1 != NULL && strchr("QqBb",q1[1]) != NULL && q1[1] != '\0'
                && q1[2] == '?') {
                    q2=q1+3;
                    q3=strstr(q2,"?=");
               }
          }
          if (q3 != NULL) {
               CHAR cs[64];
               size_t cslen=(size_t)(q1-(p+2)),n;
               const CHAR *gap=plain;
               BOOL blank=TRUE;

               for ( ; gap < p ; gap++) {
                    if (*gap != ' ' && *gap != '\t' && *gap != '\r'
                     && *gap != '\n') {
                         blank=FALSE;
                    }
               }
               if (!(lastenc && blank) && p > plain) {
                    tounicode(&w,plain,(size_t)(p-plain),0);
               }
               if (cslen >= sizeof(cs)) {
                    cslen=sizeof(cs)-1;
               }
               memcpy(cs,p+2,cslen);
               cs[cslen]='\0';
               if ((text=strchr(cs,'*')) != NULL) {
                    *text='\0';    /* RFC 2231 language suffix            */
               }
               n=(size_t)(q3-q2);
               if ((word=(CHAR *)malloc(n+1)) == NULL) {
                    w.nomem=TRUE;
                    break;
               }
               memcpy(word,q2,n);
               word[n]='\0';
               if (q1[1] == 'Q' || q1[1] == 'q') {
                    size_t i;
                    for (i=0 ; i < n ; i++) {
                         if (word[i] == '_') {
                              word[i]=' ';
                         }
                    }
                    n=ietextQPDecode(word,n);
               }
               else {
                    n=ietextB64Decode(word,n);
               }
               tounicode(&w,word,n,codepage(cs));
               free(word);
               word=NULL;
               end=q3+2;
               p=plain=end;
               lastenc=TRUE;
               continue;
          }
          if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
               lastenc=FALSE;
          }
          p++;
     }
     if (p > plain) {
          tounicode(&w,plain,(size_t)(p-plain),0);
     }
     if (!w.nomem) {
          tidy(w.s,w.len,&t,TRUE);
          if (!t.nomem && (text=tocp437(t.s,t.len,&outlen)) != NULL) {
               sbStrCopy(dst,text,dstsz);
               free(text);
          }
     }
     free(w.s);
     free(t.s);
}

size_t                             /*   decoded length                     */
ietextQPDecode(                    /* quoted-printable to bytes, in place  */
CHAR *buf,                         /*   the encoded text                   */
size_t len)                        /*   its length                         */
{
     size_t i=0,o=0;

     while (i < len) {
          if (buf[i] == '=') {
               if (i+1 < len && buf[i+1] == '\n') {
                    i+=2;          /* soft line break                      */
                    continue;
               }
               if (i+2 < len && buf[i+1] == '\r' && buf[i+2] == '\n') {
                    i+=3;
                    continue;
               }
               if (i+2 < len && hexval(buf[i+1]) >= 0 && hexval(buf[i+2]) >= 0) {
                    buf[o++]=(CHAR)(hexval(buf[i+1])*16+hexval(buf[i+2]));
                    i+=3;
                    continue;
               }
               if (i+2 == len && hexval(buf[i+1]) >= 0) {
                    break;         /* cut-off escape at the very end       */
               }
          }
          buf[o++]=buf[i++];
     }
     return(o);
}

size_t                             /*   decoded length                     */
ietextB64Decode(                   /* Base64 to bytes, in place, skipping  */
CHAR *buf,                         /*   line breaks and junk               */
size_t len)
{
     size_t i,o=0;
     ULONG acc=0;
     INT bits=0;

     for (i=0 ; i < len ; i++) {
          UCHAR c=(UCHAR)buf[i];
          INT v;
          if (c >= 'A' && c <= 'Z') v=c-'A';
          else if (c >= 'a' && c <= 'z') v=c-'a'+26;
          else if (c >= '0' && c <= '9') v=c-'0'+52;
          else if (c == '+' || c == '-') v=62;   /* '-'/'_': URL-safe form */
          else if (c == '/' || c == '_') v=63;
          else if (c == '=') break;
          else continue;
          acc=(acc << 6) | (ULONG)v;
          bits+=6;
          if (bits >= 8) {
               bits-=8;
               buf[o++]=(CHAR)((acc >> bits) & 0xFF);
          }
     }
     return(o);
}

VOID
ietextSafeName(                    /* make a DOS 8.3 file name for the BBS */
const CHAR *name,                  /*   original name (may be "")          */
const CHAR *type,                  /*   MIME type, to pick an extension    */
INT seq,                           /*   1, 2, ... used when name is unusable*/
CHAR *dst)                         /*   receives the name (13 bytes)       */
{
     static const struct { const CHAR *type, *ext; } exts[]={
          {"text/plain","TXT"},       {"text/html","HTM"},
          {"text/csv","CSV"},         {"text/calendar","ICS"},
          {"image/jpeg","JPG"},       {"image/png","PNG"},
          {"image/gif","GIF"},        {"image/bmp","BMP"},
          {"application/pdf","PDF"},  {"application/zip","ZIP"},
          {"application/x-zip-compressed","ZIP"},
          {"message/rfc822","EML"},   {"application/msword","DOC"},
          {"audio/mpeg","MP3"},       {"video/mp4","MP4"},
          {NULL,NULL}
     };
     const CHAR *base=name,*p,*dot=NULL;
     CHAR b[9],e[4];
     INT nb=0,ne=0,i;

     /* only the last path element, and only DOS-safe characters */
     for (p=name ; *p != '\0' ; p++) {
          if (*p == '/' || *p == '\\' || *p == ':') {
               base=p+1;
          }
     }
     for (p=base ; *p != '\0' ; p++) {
          if (*p == '.') {
               dot=p;
          }
     }
     for (p=base ; *p != '\0' && p != dot && nb < 8 ; p++) {
          UCHAR c=(UCHAR)toupper((UCHAR)*p);
          if (isalnum(c) && c < 0x80) {
               b[nb++]=(CHAR)c;
          }
          else if ((c == '_' || c == '-') && nb > 0) {
               b[nb++]=(CHAR)c;
          }
     }
     b[nb]='\0';
     if (dot != NULL) {
          for (p=dot+1 ; *p != '\0' && ne < 3 ; p++) {
               UCHAR c=(UCHAR)toupper((UCHAR)*p);
               if (isalnum(c) && c < 0x80) {
                    e[ne++]=(CHAR)c;
               }
          }
     }
     e[ne]='\0';
     if (nb == 0) {
          _snprintf(b,sizeof(b)-1,"ATTACH%d",seq < 1 ? 1 : seq % 100);
          b[sizeof(b)-1]='\0';
     }
     if (ne == 0) {
          sbStrCopy(e,"BIN",sizeof(e));
          for (i=0 ; exts[i].type != NULL ; i++) {
               if (type != NULL && _stricmp(type,exts[i].type) == 0) {
                    sbStrCopy(e,exts[i].ext,sizeof(e));
                    break;
               }
          }
     }
     _snprintf(dst,13,"%s.%s",b,e);
     dst[12]='\0';
}

/*
 * Windows code page for a MIME charset name, or 0 when unknown (the
 * caller then guesses).  ISO-8859-1 and US-ASCII are read as
 * Windows-1252, which is a superset that real mailers rely on.
 */
static UINT
codepage(
const CHAR *cs)
{
     static const struct { const CHAR *name; UINT cp; } names[]={
          {"utf-8",65001},      {"utf8",65001},
          /* "us-ascii" with 8-bit bytes is mislabelled; let them be guessed */
          {"us-ascii",0},       {"ascii",0},
          {"iso-8859-1",1252},  {"latin1",1252},     {"iso_8859-1",1252},
          {"iso-8859-2",28592}, {"iso-8859-3",28593},{"iso-8859-4",28594},
          {"iso-8859-5",28595}, {"iso-8859-6",28596},{"iso-8859-7",28597},
          {"iso-8859-8",28598}, {"iso-8859-9",28599},{"iso-8859-13",28603},
          {"iso-8859-15",28605},
          {"windows-1250",1250},{"windows-1251",1251},{"windows-1252",1252},
          {"windows-1253",1253},{"windows-1254",1254},{"windows-1255",1255},
          {"windows-1256",1256},{"windows-1257",1257},{"windows-1258",1258},
          {"cp1252",1252},      {"koi8-r",20866},    {"koi8-u",21866},
          {"gb2312",936},       {"gbk",936},         {"gb18030",54936},
          {"big5",950},         {"shift_jis",932},   {"euc-jp",20932},
          {"iso-2022-jp",50220},{"euc-kr",949},      {"ks_c_5601-1987",949},
          {"ibm437",437},       {"cp437",437},       {"ibm850",850},
          {NULL,0}
     };
     INT i;

     if (cs == NULL) {
          return(0);
     }
     for (i=0 ; names[i].name != NULL ; i++) {
          if (_stricmp(cs,names[i].name) == 0) {
               return(names[i].cp);
          }
     }
     return(0);
}

/*
 * Append bytes in code page cp to w as Unicode.  cp 0 = guess: valid
 * UTF-8 is read as UTF-8, anything else as Windows-1252.  A declared
 * charset that the bytes do not fit falls back to the same guess.
 */
static BOOL
tounicode(
WBUF *w,
const CHAR *src,
size_t len,
UINT cp)
{
     INT n;
     DWORD flags;

     if (len == 0) {
          return(!w->nomem);
     }
     if (len > 0x7FFFFFF0) {
          w->nomem=TRUE;
          return(FALSE);
     }
     if (cp == 0 || cp == 1252) {
          /* plain ASCII is the same in every one of these */
          size_t i;
          BOOL eight=FALSE;
          for (i=0 ; i < len ; i++) {
               if ((UCHAR)src[i] >= 0x80) {
                    eight=TRUE;
                    break;
               }
          }
          if (eight && cp == 0) {
               cp=(MultiByteToWideChar(65001,MB_ERR_INVALID_CHARS,src,
                                       (INT)len,NULL,0) > 0) ? 65001 : 1252;
          }
          else if (cp == 0) {
               cp=1252;
          }
     }
     flags=0;
     if ((n=MultiByteToWideChar(cp,flags,src,(INT)len,NULL,0)) <= 0) {
          cp=1252;         /* unknown to Windows: still show something */
          if ((n=MultiByteToWideChar(cp,0,src,(INT)len,NULL,0)) <= 0) {
               return(!w->nomem);
          }
     }
     if (w->len+(size_t)n+1 > w->cap) {
          size_t cap=w->len+(size_t)n+256;
          WCHAR *s=(WCHAR *)realloc(w->s,cap*sizeof(WCHAR));
          if (s == NULL) {
               w->nomem=TRUE;
               return(FALSE);
          }
          w->s=s;
          w->cap=cap;
     }
     MultiByteToWideChar(cp,flags,src,(INT)len,&w->s[w->len],n);
     w->len+=(size_t)n;
     w->s[w->len]=0;
     return(TRUE);
}

static VOID
wput(
WBUF *w,
WCHAR c)
{
     if (w->nomem) {
          return;
     }
     if (w->len+2 > w->cap) {
          size_t cap=(w->cap < 256) ? 256 : w->cap*2;
          WCHAR *s=(WCHAR *)realloc(w->s,cap*sizeof(WCHAR));
          if (s == NULL) {
               w->nomem=TRUE;
               return;
          }
          w->s=s;
          w->cap=cap;
     }
     w->s[w->len++]=c;
     w->s[w->len]=0;
}

static VOID
wputs(
WBUF *w,
const WCHAR *s)
{
     while (*s != 0) {
          wput(w,*s++);
     }
}

/* is the tag starting at s (after '<' or '</') named tag? */
static BOOL
istag(
const WCHAR *s,
size_t len,
const WCHAR *tag)
{
     size_t n=wcslen(tag);

     if (len < n || _wcsnicmp(s,tag,n) != 0) {
          return(FALSE);
     }
     return(len == n || !iswalnum(s[n]));
}

/* the value of attribute attr inside a tag (s .. s+len), into val */
static BOOL
attrval(
const WCHAR *s,
size_t len,
const WCHAR *attr,
WCHAR *val,
size_t valsz)
{
     size_t n=wcslen(attr),i,o=0;

     for (i=0 ; i+n < len ; i++) {
          if ((i == 0 || iswspace(s[i-1])) && _wcsnicmp(&s[i],attr,n) == 0) {
               size_t j=i+n;
               WCHAR q=0;
               while (j < len && iswspace(s[j])) j++;
               if (j >= len || s[j] != L'=') continue;
               j++;
               while (j < len && iswspace(s[j])) j++;
               if (j < len && (s[j] == L'"' || s[j] == L'\'')) {
                    q=s[j++];
               }
               while (j < len && o+1 < valsz
                   && (q != 0 ? s[j] != q : !iswspace(s[j]) && s[j] != L'>')) {
                    val[o++]=s[j++];
               }
               val[o]=0;
               return(o > 0);
          }
     }
     return(FALSE);
}

/*
 * HTML to plain text.  Not a browser: it keeps the words and the line
 * structure (paragraphs, line breaks, list items, table rows), drops
 * styles, scripts and comments, decodes entities, and shows a link's
 * address after its text when the two differ.
 */
static VOID
html2text(
const WCHAR *s,
size_t len,
WBUF *out)
{
     size_t i=0,astart=0;
     BOOL space=FALSE,pre=FALSE;
     WCHAR href[512];
     INT skip=0;                   /* inside <style>, <script>, <head>...  */

     href[0]=0;
     while (i < len) {
          WCHAR c=s[i];
          if (c == L'<') {
               size_t j,tlen;
               BOOL close;
               const WCHAR *t;

               if (i+3 < len && s[i+1] == L'!' && s[i+2] == L'-' && s[i+3] == L'-') {
                    const WCHAR *e=wcsstr(&s[i+4],L"-->");
                    i=(e == NULL) ? len : (size_t)(e-s)+3;
                    continue;
               }
               for (j=i+1 ; j < len && s[j] != L'>' ; j++) {
                    if (s[j] == L'"' || s[j] == L'\'') {
                         WCHAR q=s[j];
                         for (j++ ; j < len && s[j] != q ; j++) ;
                    }
               }
               close=(s[i+1] == L'/');
               t=&s[i+1+(close ? 1 : 0)];
               tlen=(j > (size_t)(t-s)) ? j-(size_t)(t-s) : 0;
               if (istag(t,tlen,L"style") || istag(t,tlen,L"script")
                || istag(t,tlen,L"head") || istag(t,tlen,L"title")) {
                    skip+=close ? -1 : 1;
                    if (skip < 0) skip=0;
               }
               else if (skip > 0) {
                    ;
               }
               else if (istag(t,tlen,L"br")) {
                    wput(out,L'\n');
                    space=FALSE;
               }
               else if (istag(t,tlen,L"p") || istag(t,tlen,L"div")
                     || istag(t,tlen,L"tr") || istag(t,tlen,L"table")
                     || istag(t,tlen,L"ul") || istag(t,tlen,L"ol")
                     || istag(t,tlen,L"blockquote")
                     || istag(t,tlen,L"h1") || istag(t,tlen,L"h2")
                     || istag(t,tlen,L"h3") || istag(t,tlen,L"h4")
                     || istag(t,tlen,L"h5") || istag(t,tlen,L"h6")) {
                    wput(out,L'\n');
                    if (istag(t,tlen,L"p") || t[0] == L'h' || t[0] == L'H') {
                         wput(out,L'\n');
                    }
                    space=FALSE;
               }
               else if (istag(t,tlen,L"li") && !close) {
                    wputs(out,L"\n* ");
                    space=FALSE;
               }
               else if (istag(t,tlen,L"hr")) {
                    wputs(out,L"\n----------------------------------------\n");
                    space=FALSE;
               }
               else if (istag(t,tlen,L"td") || istag(t,tlen,L"th")) {
                    if (!close) {
                         space=TRUE;
                    }
               }
               else if (istag(t,tlen,L"pre")) {
                    pre=!close;
                    wput(out,L'\n');
               }
               else if (istag(t,tlen,L"a")) {
                    if (!close) {
                         if (!attrval(t,tlen,L"href",href,sizeof(href)/sizeof(href[0]))) {
                              href[0]=0;
                         }
                         astart=out->len;
                    }
                    else if (href[0] != 0
                          && (_wcsnicmp(href,L"http",4) == 0
                           || _wcsnicmp(href,L"mailto:",7) == 0)) {
                         /* show the address unless the text already is it */
                         const WCHAR *shown=href;
                         size_t tl;

                         /* the space before the link is written late */
                         while (astart < out->len && out->s[astart] == L' ') {
                              astart++;
                         }
                         tl=out->len-astart;
                         if (_wcsnicmp(href,L"mailto:",7) == 0) {
                              shown=href+7;
                         }
                         if (tl == 0 || wcsncmp(&out->s[astart],shown,tl) != 0
                          || wcslen(shown) != tl) {
                              wputs(out,L" <");
                              wputs(out,shown);
                              wput(out,L'>');
                         }
                         href[0]=0;
                    }
               }
               i=(j < len) ? j+1 : len;
               continue;
          }
          if (skip > 0) {
               i++;
               continue;
          }
          if (c == L'&') {
               size_t used;
               WCHAR e=entity(&s[i],len-i,&used);
               if (used > 0) {
                    if (space && out->len > 0 && out->s[out->len-1] != L'\n') {
                         wput(out,L' ');
                    }
                    space=FALSE;
                    wput(out,e);
                    i+=used;
                    continue;
               }
          }
          if (!pre && (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n')) {
               space=TRUE;
               i++;
               continue;
          }
          if (space && out->len > 0 && out->s[out->len-1] != L'\n') {
               wput(out,L' ');
          }
          space=FALSE;
          wput(out,c);
          i++;
     }
}

/* decode one HTML entity at s ("&amp;"); used = characters consumed */
static WCHAR
entity(
const WCHAR *s,
size_t len,
size_t *used)
{
     static const struct { const WCHAR *name; WCHAR c; } names[]={
          {L"amp",L'&'},     {L"lt",L'<'},      {L"gt",L'>'},
          {L"quot",L'"'},    {L"apos",L'\''},   {L"nbsp",0x00A0},
          {L"copy",0x00A9},  {L"reg",0x00AE},   {L"trade",0x2122},
          {L"mdash",0x2014}, {L"ndash",0x2013}, {L"hellip",0x2026},
          {L"lsquo",0x2018}, {L"rsquo",0x2019}, {L"ldquo",0x201C},
          {L"rdquo",0x201D}, {L"bull",0x2022},  {L"middot",0x00B7},
          {L"deg",0x00B0},   {L"euro",0x20AC},  {L"pound",0x00A3},
          {L"cent",0x00A2},  {L"yen",0x00A5},   {L"sect",0x00A7},
          {L"laquo",0x00AB}, {L"raquo",0x00BB}, {L"times",0x00D7},
          {L"eacute",0x00E9},{L"egrave",0x00E8},{L"aacute",0x00E1},
          {L"agrave",0x00E0},{L"uuml",0x00FC},  {L"ouml",0x00F6},
          {L"auml",0x00E4},  {L"ntilde",0x00F1},{L"ccedil",0x00E7},
          {L"zwnj",0x200C},  {L"zwj",0x200D},   {L"shy",0x00AD},
          {NULL,0}
     };
     size_t i,end;
     INT k;

     *used=0;
     for (end=1 ; end < len && end < 12 && s[end] != L';' ; end++) ;
     if (end >= len || s[end] != L';') {
          return(0);
     }
     if (s[1] == L'#') {
          ULONG v=0;
          BOOL hex=(s[2] == L'x' || s[2] == L'X');
          for (i=hex ? 3 : 2 ; i < end ; i++) {
               INT d=hex ? hexval(s[i]) : (iswdigit(s[i]) ? s[i]-L'0' : -1);
               if (d < 0) {
                    return(0);
               }
               v=v*(hex ? 16 : 10)+(ULONG)d;
          }
          *used=end+1;
          return((v == 0 || v > 0xFFFF) ? L'?' : (WCHAR)v);
     }
     for (k=0 ; names[k].name != NULL ; k++) {
          size_t n=wcslen(names[k].name);
          if (n == end-1 && wcsncmp(&s[1],names[k].name,n) == 0) {
               *used=end+1;
               return(names[k].c);
          }
     }
     return(0);
}

/*
 * Make text safe and plain: one '\n' per line break, typographic
 * characters swapped for ones CP437 has, control characters (including
 * ESC, so no ANSI sequences) removed, trailing spaces dropped and runs of
 * blank lines cut to one.  oneline = for headers: line breaks become
 * spaces.
 */
static VOID
tidy(
const WCHAR *s,
size_t len,
WBUF *out,
BOOL oneline)
{
     size_t i;
     INT blank=0;
     size_t col=0;

     for (i=0 ; i < len ; i++) {
          WCHAR c=s[i];
          const WCHAR *rep=NULL;

          if (c == L'\r') {
               if (i+1 < len && s[i+1] == L'\n') {
                    continue;
               }
               c=L'\n';
          }
          if (c == L'\n') {
               if (oneline) {
                    c=L' ';
               }
               else {
                    /* drop trailing spaces, keep at most one blank line */
                    while (out->len > 0 && out->s[out->len-1] == L' ') {
                         out->len--;
                    }
                    if (out->len == 0 || out->s[out->len-1] == L'\n') {
                         if (++blank > 1 || out->len == 0) {
                              continue;
                         }
                    }
                    else {
                         blank=0;
                    }
                    wput(out,L'\n');
                    col=0;
                    continue;
               }
          }
          if (c >= 0xD800 && c <= 0xDBFF) {
               if (i+1 < len && s[i+1] >= 0xDC00 && s[i+1] <= 0xDFFF) {
                    i++;           /* emoji etc.: one '?' for the pair     */
               }
               c=L'?';
          }
          switch (c) {
          case 0x2018: case 0x2019: case 0x201A: case 0x2032: c=L'\''; break;
          case 0x201C: case 0x201D: case 0x201E: case 0x2033: c=L'"'; break;
          case 0x2010: case 0x2011: case 0x2012: case 0x2013:
          case 0x2014: case 0x2015: case 0x2212: c=L'-'; break;
          case 0x2022: case 0x25CF: case 0x2023: c=L'*'; break;
          case 0x00A0: case 0x2002: case 0x2003: case 0x2004: case 0x2005:
          case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A:
          case 0x202F: case 0x205F: case 0x3000: c=L' '; break;
          case 0x2026: rep=L"..."; break;
          case 0x2122: rep=L"(TM)"; break;
          case 0x00A9: rep=L"(C)"; break;
          case 0x00AE: rep=L"(R)"; break;
          case 0x20AC: rep=L"EUR"; break;
          case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF:
          case 0xFE0E: case 0xFE0F: case 0x00AD: continue;
          case L'\t':
               if (oneline) {
                    c=L' ';
                    break;
               }
               do {
                    wput(out,L' ');
               } while (++col % 8 != 0);
               continue;
          }
          if (rep == NULL && (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0))) {
               continue;       /* control characters, ESC included */
          }
          if (oneline && c == L' ' && (out->len == 0 || out->s[out->len-1] == L' ')) {
               continue;
          }
          if (rep != NULL) {
               wputs(out,rep);
               col+=wcslen(rep);
          }
          else {
               wput(out,c);
               col++;
          }
     }
     while (out->len > 0 && (out->s[out->len-1] == L' '
                          || out->s[out->len-1] == L'\n')) {
          out->len--;
     }
     if (out->s != NULL) {
          out->s[out->len]=0;
     }
     else {
          wput(out,0);
          out->len=0;
     }
}

/*
 * Word-wrap at col.  Breaks at the last space that fits.  A word longer
 * than a whole line (a long web address) is never cut, and is not left
 * stranded either: it stays on the line it starts on unless that line
 * already has plenty of text, and a short tail after it (" )", ".")
 * stays with it.  Such a line is simply longer than col.
 */
static VOID
wrap(
const WCHAR *s,
size_t len,
INT col,
WBUF *out)
{
     size_t i=0;

     while (i < len) {
          size_t e=i,n,brk;

          while (e < len && s[e] != L'\n') {
               e++;
          }
          while ((n=e-i) > (size_t)col) {
               size_t ws,we;

               for (brk=i+col ; brk > i && s[brk] != L' ' ; brk--) ;
               /* the word that runs past the column */
               ws=(brk == i) ? i : brk+1;
               for (we=ws ; we < e && s[we] != L' ' ; we++) ;
               if (we-ws > (size_t)col && (brk == i || brk-i < (size_t)col/2)) {
                    brk=(e-we <= 10) ? e : we;
                    if (brk == e) {
                         break;
                    }
               }
               else if (brk == i) {
                    for (brk=i+col ; brk < e && s[brk] != L' ' ; brk++) ;
                    if (brk == e) {
                         break;
                    }
               }
               for (n=i ; n < brk ; n++) {
                    wput(out,s[n]);
               }
               wput(out,L'\n');
               for (i=brk ; i < e && s[i] == L' ' ; i++) ;
          }
          for ( ; i < e ; i++) {
               wput(out,s[i]);
          }
          if (e < len) {
               wput(out,L'\n');
          }
          i=e+1;
     }
     if (out->s == NULL) {
          wput(out,0);
          out->len=0;
     }
}

/*
 * Unicode to CP437, closest match per character ('?' when there is
 * none).  Any byte that comes out as a control code is replaced too.
 */
static CHAR *
tocp437(
const WCHAR *s,
size_t len,
size_t *outlen)
{
     CHAR *out;
     INT n;
     size_t i;

     *outlen=0;
     if (len == 0) {
          if ((out=(CHAR *)malloc(1)) != NULL) {
               out[0]='\0';
          }
          return(out);
     }
     n=WideCharToMultiByte(437,0,s,(INT)len,NULL,0,"?",NULL);
     if (n <= 0 || (out=(CHAR *)malloc((size_t)n+1)) == NULL) {
          return(NULL);
     }
     WideCharToMultiByte(437,0,s,(INT)len,out,n,"?",NULL);
     out[n]='\0';
     for (i=0 ; i < (size_t)n ; i++) {
          if (((UCHAR)out[i] < 0x20 && out[i] != '\n') || (UCHAR)out[i] == 0x7F) {
               out[i]='?';
          }
     }
     *outlen=(size_t)n;
     return(out);
}

static INT
hexval(
INT c)
{
     if (c >= '0' && c <= '9') return(c-'0');
     if (c >= 'A' && c <= 'F') return(c-'A'+10);
     if (c >= 'a' && c <= 'f') return(c-'a'+10);
     return(-1);
}
