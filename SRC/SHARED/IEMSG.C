/***************************************************************************
 *                                                                         *
 *   IEMSG.C                                                               *
 *                                                                         *
 *   Better Internet Email - mail receiver (SNBIEMLR) for The Major BBS v10*
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   Decodes one Internet mail message for import into the BBS.            *
 *                                                                         *
 *   WHO A MESSAGE IS FOR.  Every message in the catch-all mailbox says    *
 *   "Delivered-To: <the catch-all>", so that header is useless.  The     *
 *   mail host files one copy per recipient and stamps each copy's top     *
 *   Received: header with "for <the real recipient>".  To: and Cc: are   *
 *   NOT used when that clause is present: a message sent to two users of  *
 *   the board arrives as two copies that both list both users, so going  *
 *   by To: would give each user the message twice.  To:/Cc: are only a   *
 *   last resort.                                                          *
 *                                                                         *
 *   WHAT THE USER READS.  MIME parts are walked recursively.  In a       *
 *   "pick one" (multipart/alternative) group the plain-text version is   *
 *   preferred, HTML converted to text otherwise.  Pictures embedded in an *
 *   HTML layout (multipart/related) and signatures are skipped; every     *
 *   real attachment is kept.                                              *
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

#define MAXDEPTH  12               /* deepest MIME nesting we follow       */
#define MAXPARTS  500              /* most MIME parts we look at           */

typedef struct walk {              /* state while walking MIME parts       */
     IEMAIL *m;
     INT wrapcol;
     INT parts;                    /*   parts seen so far                  */
     BOOL nomem;                   /*   an allocation failed               */
} WALK;

typedef struct ctype {             /* a parsed Content-Type                */
     CHAR type[64];                /*   "text/plain", lower case           */
     CHAR charset[64];
     CHAR boundary[128];
     CHAR name[IE_FNAMESZ];        /*   name= parameter, decoded           */
} CTYPE;

static VOID entity(WALK *w, const CHAR *ent, size_t len, const CHAR *deftype,
                   INT depth);
static size_t hdrend(const CHAR *raw, size_t len, size_t *bodyoff);
static VOID parsectype(const CHAR *value, const CHAR *deftype, CTYPE *ct);
static BOOL param(const CHAR *value, const CHAR *key, CHAR *out, size_t outsz);
static VOID filename(const CHAR *disp, const CTYPE *ct, CHAR *out,
                     size_t outsz);
static VOID addtext(WALK *w, const CHAR *data, size_t len, const CTYPE *ct);
static VOID addatt(WALK *w, CHAR *data, size_t len, const CTYPE *ct,
                   const CHAR *name);
static CHAR *decode(const CHAR *body, size_t len, const CHAR *cte,
                    size_t *outlen);
static BOOL isautogen(const CHAR *hdr, size_t hdrlen, const IEMAIL *m);
static VOID route(const CHAR *hdr, size_t hdrlen, IEMAIL *m);
static VOID addrcpt(IEMAIL *m, const CHAR *addr);
static VOID trim(CHAR *s);
static VOID lower(CHAR *s);

BOOL                               /*   FALSE only when out of memory      */
iemailParse(                       /* decode a whole message               */
const CHAR *raw,                   /*   the message as downloaded (CRLF)   */
size_t len,                        /*   its length                         */
INT wrapcol,                       /*   word-wrap column (0 = no wrap)     */
IEMAIL *m)                         /*   receives the result (iemailFree it)*/
{
     WALK w;
     size_t hlen,boff;
     CHAR *v,name[IE_NAMESZ];
     CHAR addrs[1][IE_ADRSZ];

     memset(m,0,sizeof(*m));
     memset(&w,0,sizeof(w));
     w.m=m;
     w.wrapcol=wrapcol;
     hlen=hdrend(raw,len,&boff);
     if ((m->header=(CHAR *)malloc(hlen+1)) == NULL) {
          return(FALSE);
     }
     memcpy(m->header,raw,hlen);
     m->header[hlen]='\0';

     route(raw,hlen,m);
     if ((v=iemailHeader(raw,hlen,"From",0)) != NULL) {
          name[0]='\0';
          if (iemailAddrList(v,addrs,1,name,sizeof(name)) > 0) {
               sbStrCopy(m->from,addrs[0],sizeof(m->from));
          }
          ietextDecodeWords(name,m->fromname,sizeof(m->fromname));
          free(v);
     }
     if ((v=iemailHeader(raw,hlen,"Reply-To",0)) != NULL) {
          if (iemailAddrList(v,addrs,1,NULL,0) > 0) {
               sbStrCopy(m->replyto,addrs[0],sizeof(m->replyto));
          }
          free(v);
     }
     if ((v=iemailHeader(raw,hlen,"Return-Path",0)) != NULL) {
          if (iemailAddrList(v,addrs,1,NULL,0) > 0) {
               sbStrCopy(m->retpath,addrs[0],sizeof(m->retpath));
          }
          free(v);
     }
     if ((v=iemailHeader(raw,hlen,"Subject",0)) != NULL) {
          ietextDecodeWords(v,m->subject,sizeof(m->subject));
          free(v);
     }
     if ((v=iemailHeader(raw,hlen,"Date",0)) != NULL) {
          sbStrCopy(m->date,v,sizeof(m->date));
          free(v);
     }
     if ((v=iemailHeader(raw,hlen,"Message-ID",0)) != NULL) {
          CHAR *s=v,*e;
          while (*s == ' ' || *s == '<') s++;
          if ((e=strchr(s,'>')) != NULL) *e='\0';
          sbStrCopy(m->msgid,s,sizeof(m->msgid));
          trim(m->msgid);
          free(v);
     }
     m->autogen=isautogen(raw,hlen,m);

     /* the whole message is the top-level MIME entity */
     entity(&w,raw,len,"text/plain",0);
     if (w.nomem) {
          return(FALSE);
     }
     if (m->text == NULL) {
          if ((m->text=(CHAR *)malloc(1)) == NULL) {
               return(FALSE);
          }
          m->text[0]='\0';
          m->textlen=0;
     }
     return(TRUE);
}

VOID
iemailFree(                        /* release everything iemailParse made  */
IEMAIL *m)
{
     INT i;

     free(m->text);
     free(m->header);
     for (i=0 ; i < m->natt ; i++) {
          free(m->att[i].data);
     }
     memset(m,0,sizeof(*m));
}

CHAR *                             /*   value (malloc'd, unfolded) or NULL */
iemailHeader(                      /* find a header in a header block      */
const CHAR *hdr,                   /*   header block (CRLF or LF lines)    */
size_t hdrlen,                     /*   its length                         */
const CHAR *name,                  /*   header name, e.g. "Subject"        */
INT which)                         /*   0 = first, 1 = second, ...         */
{
     size_t nlen=strlen(name),i=0;

     while (i < hdrlen) {
          size_t eol=i;

          while (eol < hdrlen && hdr[eol] != '\n') {
               eol++;
          }
          if (hdr[i] != ' ' && hdr[i] != '\t' && eol-i > nlen
           && _strnicmp(&hdr[i],name,nlen) == 0 && hdr[i+nlen] == ':'
           && which-- == 0) {
               /* the value, with its continuation lines unfolded */
               size_t end=eol,o=0,j;
               CHAR *v;

               while (end+1 < hdrlen && (hdr[end+1] == ' ' || hdr[end+1] == '\t')) {
                    for (end++ ; end < hdrlen && hdr[end] != '\n' ; end++) ;
               }
               if ((v=(CHAR *)malloc(end-i+1)) == NULL) {
                    return(NULL);
               }
               for (j=i+nlen+1 ; j < end && j < hdrlen ; j++) {
                    if (hdr[j] != '\r' && hdr[j] != '\n') {
                         v[o++]=(hdr[j] == '\t') ? ' ' : hdr[j];
                    }
               }
               v[o]='\0';
               trim(v);
               return(v);
          }
          i=eol+1;
     }
     return(NULL);
}

BOOL                               /*   TRUE if an address was found       */
iemailReceivedFor(                 /* the "for <addr>" clause of a         */
const CHAR *rcv,                   /*   Received: header value             */
CHAR *addr,                        /*   receives the address               */
size_t addrsz)
{
     INT depth=0;
     size_t i,n=strlen(rcv);

     addr[0]='\0';
     for (i=0 ; i < n ; i++) {
          CHAR c=rcv[i];
          if (c == '(') {
               depth++;            /* comments can say anything; skip them */
          }
          else if (c == ')' && depth > 0) {
               depth--;
          }
          else if (c == ';' && depth == 0) {
               break;              /* the date follows; no clause here     */
          }
          else if (depth == 0 && (i == 0 || rcv[i-1] == ' ')
                && _strnicmp(&rcv[i],"for",3) == 0
                && (rcv[i+3] == ' ' || rcv[i+3] == '<')) {
               size_t j=i+3,o=0;
               while (rcv[j] == ' ') j++;
               if (rcv[j] == '<') {
                    for (j++ ; rcv[j] != '\0' && rcv[j] != '>' && o+1 < addrsz ; j++) {
                         addr[o++]=rcv[j];
                    }
               }
               else {
                    for ( ; rcv[j] != '\0' && rcv[j] != ' ' && rcv[j] != ';'
                         && o+1 < addrsz ; j++) {
                         addr[o++]=rcv[j];
                    }
               }
               addr[o]='\0';
               trim(addr);
               if (strchr(addr,'@') != NULL && strchr(addr,' ') == NULL) {
                    lower(addr);
                    return(TRUE);
               }
               addr[0]='\0';
          }
     }
     return(FALSE);
}

INT                                /*   number of addresses found          */
iemailAddrList(                    /* split an address header value        */
const CHAR *value,                 /*   e.g. "Ann <a@x>, \"B, C\" <b@x>"   */
CHAR (*addrs)[IE_ADRSZ],           /*   receives the bare addresses        */
INT max,                           /*   room in addrs                      */
CHAR *firstname,                   /*   receives the first display name    */
size_t namesz)
{
     const CHAR *p=value;
     INT cnt=0;

     if (firstname != NULL && namesz > 0) {
          firstname[0]='\0';
     }
     while (*p != '\0' && cnt < max) {
          CHAR name[IE_NAMESZ],adr[IE_ADRSZ],bare[IE_ADRSZ];
          size_t nn=0,na=0,nb=0;
          BOOL inq=FALSE,inang=FALSE,hadang=FALSE;
          INT depth=0;

          name[0]=adr[0]=bare[0]='\0';
          /* one element: up to a comma that is not quoted or bracketed */
          for ( ; *p != '\0' ; p++) {
               CHAR c=*p;
               if (inq) {
                    if (c == '\\' && p[1] != '\0') {
                         p++;
                         if (nn+1 < sizeof(name)) name[nn++]=*p;
                    }
                    else if (c == '"') {
                         inq=FALSE;
                    }
                    else if (nn+1 < sizeof(name)) {
                         name[nn++]=c;
                    }
                    continue;
               }
               if (depth > 0) {
                    if (c == '(') depth++;
                    else if (c == ')') depth--;
                    continue;
               }
               if (c == '(') {
                    depth++;
               }
               else if (c == '"') {
                    inq=TRUE;
               }
               else if (c == '<') {
                    inang=hadang=TRUE;
                    na=0;
               }
               else if (c == '>') {
                    inang=FALSE;
               }
               else if (inang) {
                    if (na+1 < sizeof(adr)) adr[na++]=c;
               }
               else if (c == ',' || c == ';') {
                    p++;
                    break;
               }
               else if (c == ':' && !hadang && strchr(bare,'@') == NULL) {
                    nb=0;          /* "group name:" - what follows counts  */
                    nn=0;
               }
               else {
                    if (nb+1 < sizeof(bare)) bare[nb++]=c;
                    bare[nb]='\0';
                    if (nn+1 < sizeof(name)) name[nn++]=c;
               }
               bare[nb]='\0';
          }
          name[nn]='\0';
          adr[na]='\0';
          bare[nb]='\0';
          if (!hadang) {
               sbStrCopy(adr,bare,sizeof(adr));
               name[0]='\0';
          }
          trim(adr);
          trim(name);
          if (adr[0] != '\0' && strchr(adr,'@') != NULL && strchr(adr,' ') == NULL) {
               sbStrCopy(addrs[cnt],adr,IE_ADRSZ);
               if (cnt == 0 && firstname != NULL && namesz > 0) {
                    sbStrCopy(firstname,name,namesz);
               }
               cnt++;
          }
     }
     return(cnt);
}

/*
 * One MIME entity: a header block, a blank line, and a body.  Multipart
 * bodies are split at their boundary lines and each part handled in turn.
 */
static VOID
entity(
WALK *w,
const CHAR *ent,
size_t len,
const CHAR *deftype,
INT depth)
{
     size_t hlen,boff;
     CHAR *v,*cte=NULL,*disp=NULL,fname[IE_FNAMESZ];
     CTYPE ct;
     const CHAR *body;
     size_t blen;

     if (w->nomem || depth > MAXDEPTH || ++w->parts > MAXPARTS) {
          return;
     }
     hlen=hdrend(ent,len,&boff);
     body=ent+boff;
     blen=len-boff;
     v=iemailHeader(ent,hlen,"Content-Type",0);
     parsectype(v,deftype,&ct);
     free(v);
     cte=iemailHeader(ent,hlen,"Content-Transfer-Encoding",0);
     disp=iemailHeader(ent,hlen,"Content-Disposition",0);
     filename(disp,&ct,fname,sizeof(fname));

     if (strncmp(ct.type,"multipart/",10) == 0 && ct.boundary[0] != '\0') {
          /* find each "--boundary" line; the part runs to the next one */
          const CHAR *starts[MAXPARTS];
          size_t lens[MAXPARTS];
          INT n=0,i,pick=-1;
          size_t bl=strlen(ct.boundary),pos=0,partstart=0;
          BOOL open=FALSE;

          while (pos+2+bl <= blen && n < MAXPARTS) {
               if ((pos == 0 || body[pos-1] == '\n')
                && body[pos] == '-' && body[pos+1] == '-'
                && memcmp(&body[pos+2],ct.boundary,bl) == 0
                && (pos+2+bl == blen || strchr("-\r\n \t",body[pos+2+bl]) != NULL)) {
                    size_t eol=pos+2+bl;
                    BOOL last=(eol+1 < blen && body[eol] == '-' && body[eol+1] == '-');
                    if (open) {
                         size_t end=pos;
                         if (end > partstart && body[end-1] == '\n') end--;
                         if (end > partstart && body[end-1] == '\r') end--;
                         starts[n]=&body[partstart];
                         lens[n++]=end-partstart;
                    }
                    while (eol < blen && body[eol] != '\n') eol++;
                    partstart=(eol < blen) ? eol+1 : blen;
                    open=!last;
                    if (last) {
                         break;
                    }
                    pos=partstart;
                    continue;
               }
               while (pos < blen && body[pos] != '\n') pos++;
               pos++;
          }
          if (open && partstart < blen && n < MAXPARTS) {
               starts[n]=&body[partstart];   /* no closing line: to the end */
               lens[n++]=blen-partstart;
          }

          if (strcmp(ct.type,"multipart/alternative") == 0) {
               /* same content in several forms: plain text, else HTML */
               INT html=-1,multi=-1;
               for (i=0 ; i < n ; i++) {
                    CTYPE c;
                    size_t ph,pb;
                    ph=hdrend(starts[i],lens[i],&pb);
                    v=iemailHeader(starts[i],ph,"Content-Type",0);
                    parsectype(v,"text/plain",&c);
                    free(v);
                    if (strcmp(c.type,"text/plain") == 0 && pick < 0) pick=i;
                    else if (strcmp(c.type,"text/html") == 0 && html < 0) html=i;
                    else if (strncmp(c.type,"multipart/",10) == 0 && multi < 0) multi=i;
               }
               if (pick < 0) pick=(html >= 0) ? html : (multi >= 0) ? multi : 0;
               if (n > 0) {
                    entity(w,starts[pick],lens[pick],"text/plain",depth+1);
               }
          }
          else if (strcmp(ct.type,"multipart/related") == 0
                || strcmp(ct.type,"multipart/signed") == 0) {
               /* the first part is the content; the rest are pictures it */
               /* shows, or the signature - only real attachments count  */
               for (i=0 ; i < n ; i++) {
                    if (i == 0) {
                         entity(w,starts[i],lens[i],"text/plain",depth+1);
                    }
                    else {
                         size_t ph,pb;
                         ph=hdrend(starts[i],lens[i],&pb);
                         v=iemailHeader(starts[i],ph,"Content-Disposition",0);
                         if (v != NULL && _strnicmp(v,"attachment",10) == 0) {
                              entity(w,starts[i],lens[i],"text/plain",depth+1);
                         }
                         free(v);
                    }
               }
          }
          else {
               /* mixed, report, digest...: every part counts */
               const CHAR *dt=(strcmp(ct.type,"multipart/digest") == 0)
                            ? "message/rfc822" : "text/plain";
               for (i=0 ; i < n ; i++) {
                    entity(w,starts[i],lens[i],dt,depth+1);
               }
          }
     }
     else {
          BOOL isatt;
          size_t dlen;
          CHAR *data;
          BOOL text=(strcmp(ct.type,"text/plain") == 0
                  || strcmp(ct.type,"text/html") == 0
                  || strcmp(ct.type,"text/enriched") == 0);

          if (strcmp(ct.type,"application/pgp-signature") == 0
           || strcmp(ct.type,"application/pkcs7-signature") == 0
           || strcmp(ct.type,"application/x-pkcs7-signature") == 0) {
               goto done;          /* a signature is not for reading       */
          }
          if (disp != NULL && _strnicmp(disp,"attachment",10) == 0) {
               isatt=TRUE;
          }
          else {
               isatt=!text;        /* inline text is part of the message   */
          }
          if ((data=decode(body,blen,cte,&dlen)) == NULL) {
               w->nomem=TRUE;
               goto done;
          }
          if (isatt) {
               addatt(w,data,dlen,&ct,fname);    /* keeps data */
          }
          else {
               addtext(w,data,dlen,&ct);
               free(data);
          }
     }
done:
     free(cte);
     free(disp);
}

/*
 * Length of the header block; *bodyoff gets where the body starts (after
 * the blank line).  A part with no headers starts with the blank line.
 */
static size_t
hdrend(
const CHAR *raw,
size_t len,
size_t *bodyoff)
{
     size_t i;

     if (len >= 2 && raw[0] == '\r' && raw[1] == '\n') {
          *bodyoff=2;
          return(0);
     }
     if (len >= 1 && raw[0] == '\n') {
          *bodyoff=1;
          return(0);
     }
     for (i=0 ; i+1 < len ; i++) {
          if (raw[i] == '\n') {
               if (raw[i+1] == '\n') {
                    *bodyoff=i+2;
                    return(i+1);
               }
               if (raw[i+1] == '\r' && i+2 < len && raw[i+2] == '\n') {
                    *bodyoff=i+3;
                    return(i+1);
               }
          }
     }
     *bodyoff=len;                 /* headers only, no body                */
     return(len);
}

static VOID
parsectype(
const CHAR *value,
const CHAR *deftype,
CTYPE *ct)
{
     size_t i;

     memset(ct,0,sizeof(*ct));
     if (value == NULL || value[0] == '\0') {
          sbStrCopy(ct->type,deftype,sizeof(ct->type));
          return;
     }
     for (i=0 ; value[i] != '\0' && value[i] != ';' && value[i] != ' '
                && i+1 < sizeof(ct->type) ; i++) {
          ct->type[i]=(CHAR)tolower((UCHAR)value[i]);
     }
     ct->type[i]='\0';
     if (strchr(ct->type,'/') == NULL) {
          sbStrCopy(ct->type,deftype,sizeof(ct->type));
     }
     param(value,"charset",ct->charset,sizeof(ct->charset));
     param(value,"boundary",ct->boundary,sizeof(ct->boundary));
     param(value,"name",ct->name,sizeof(ct->name));
}

/*
 * A parameter of a Content-Type/Content-Disposition value.  Handles
 * quoted values and the RFC 2231 forms (name*=utf-8''..., and
 * name*0=/name*1= continuations); the result is CP437.
 */
static BOOL
param(
const CHAR *value,
const CHAR *key,
CHAR *out,
size_t outsz)
{
     CHAR raw[512],cs[64];
     size_t klen=strlen(key),o=0;
     BOOL ext=FALSE,found=FALSE;
     INT seg;

     out[0]='\0';
     cs[0]='\0';
     for (seg=-1 ; seg < 64 ; seg++) {
          const CHAR *p=value;
          BOOL got=FALSE;

          while ((p=strchr(p,';')) != NULL) {
               const CHAR *k;
               CHAR suffix[16];
               BOOL star=FALSE;

               for (p++ ; *p == ' ' ; p++) ;
               k=p;
               if (_strnicmp(k,key,klen) != 0) {
                    continue;
               }
               k+=klen;
               if (seg < 0) {
                    suffix[0]='\0';
               }
               else {
                    _snprintf(suffix,sizeof(suffix)-1,"*%d",seg);
                    suffix[sizeof(suffix)-1]='\0';
               }
               if (strncmp(k,suffix,strlen(suffix)) != 0) {
                    continue;
               }
               k+=strlen(suffix);
               if (*k == '*') {
                    star=TRUE;
                    k++;
               }
               while (*k == ' ') k++;
               if (*k != '=') {
                    continue;
               }
               for (k++ ; *k == ' ' ; k++) ;
               if (*k == '"') {
                    for (k++ ; *k != '\0' && *k != '"' && o+1 < sizeof(raw) ; k++) {
                         if (*k == '\\' && k[1] != '\0') k++;
                         raw[o++]=*k;
                    }
               }
               else {
                    const CHAR *start=k;
                    if (star && seg <= 0) {
                         /* the first piece starts charset'language'value */
                         const CHAR *tokend=k+strcspn(k,"; ");
                         const CHAR *q1=(const CHAR *)memchr(k,'\'',tokend-k);
                         const CHAR *q2=(q1 == NULL) ? NULL
                              : (const CHAR *)memchr(q1+1,'\'',tokend-(q1+1));
                         if (q2 != NULL) {
                              size_t n=(size_t)(q1-k);
                              if (n >= sizeof(cs)) n=sizeof(cs)-1;
                              memcpy(cs,k,n);
                              cs[n]='\0';
                              start=q2+1;
                         }
                    }
                    for (k=start ; *k != '\0' && *k != ';' && *k != ' '
                                 && o+1 < sizeof(raw) ; k++) {
                         if (star && *k == '%' && isxdigit((UCHAR)k[1])
                          && isxdigit((UCHAR)k[2])) {
                              CHAR hx[3]={k[1],k[2],'\0'};
                              raw[o++]=(CHAR)strtol(hx,NULL,16);
                              k+=2;
                         }
                         else {
                              raw[o++]=*k;
                         }
                    }
               }
               if (star) {
                    ext=TRUE;
               }
               got=found=TRUE;
               break;
          }
          if (seg < 0 && got) {
               break;              /* a plain key=value: no segments       */
          }
          if (seg >= 0 && !got) {
               break;
          }
     }
     raw[o]='\0';
     if (!found) {
          return(FALSE);
     }
     if (_stricmp(key,"boundary") == 0 || _stricmp(key,"charset") == 0) {
          sbStrCopy(out,raw,outsz);
     }
     else if (ext) {
          size_t n;
          CHAR *t=ietextToCp437(raw,o,cs,FALSE,0,&n);
          if (t != NULL) {
               sbStrCopy(out,t,outsz);
               free(t);
          }
     }
     else {
          ietextDecodeWords(raw,out,outsz);  /* name="=?UTF-8?B?...?=" */
     }
     return(TRUE);
}

static VOID
filename(
const CHAR *disp,
const CTYPE *ct,
CHAR *out,
size_t outsz)
{
     out[0]='\0';
     if (disp != NULL) {
          CHAR buf[1024];
          /* param() expects "type; key=value", which disposition matches */
          sbStrCopy(buf,disp,sizeof(buf));
          param(buf,"filename",out,outsz);
     }
     if (out[0] == '\0') {
          sbStrCopy(out,ct->name,outsz);
     }
}

static VOID
addtext(
WALK *w,
const CHAR *data,
size_t len,
const CTYPE *ct)
{
     IEMAIL *m=w->m;
     BOOL html=(strcmp(ct->type,"text/html") == 0);
     size_t n;
     CHAR *t,*bigger;

     if ((t=ietextToCp437(data,len,ct->charset,html,w->wrapcol,&n)) == NULL) {
          w->nomem=TRUE;
          return;
     }
     if (n == 0) {
          free(t);
          return;
     }
     if (html) {
          m->fromhtml=TRUE;
     }
     if (m->text == NULL) {
          m->text=t;
          m->textlen=n;
          return;
     }
     /* a second text part (rare): add it after a blank line */
     if ((bigger=(CHAR *)realloc(m->text,m->textlen+2+n+1)) == NULL) {
          free(t);
          w->nomem=TRUE;
          return;
     }
     m->text=bigger;
     memcpy(&m->text[m->textlen],"\n\n",2);
     memcpy(&m->text[m->textlen+2],t,n+1);
     m->textlen+=2+n;
     free(t);
}

static VOID
addatt(
WALK *w,
CHAR *data,
size_t len,
const CTYPE *ct,
const CHAR *name)
{
     IEMAIL *m=w->m;
     IEATT *a;

     if (m->natt >= IE_MAXATT) {
          m->dropped++;
          free(data);
          return;
     }
     a=&m->att[m->natt++];
     sbStrCopy(a->name,name,sizeof(a->name));
     sbStrCopy(a->type,ct->type,sizeof(a->type));
     a->data=data;
     a->len=len;
}

/*
 * Undo the Content-Transfer-Encoding.  Always returns a malloc'd copy
 * (NULL only when out of memory).
 */
static CHAR *
decode(
const CHAR *body,
size_t len,
const CHAR *cte,
size_t *outlen)
{
     CHAR *d;

     if ((d=(CHAR *)malloc(len+1)) == NULL) {
          return(NULL);
     }
     memcpy(d,body,len);
     d[len]='\0';
     if (cte != NULL && _strnicmp(cte,"base64",6) == 0) {
          len=ietextB64Decode(d,len);
     }
     else if (cte != NULL && _strnicmp(cte,"quoted-printable",16) == 0) {
          len=ietextQPDecode(d,len);
     }
     d[len]='\0';
     *outlen=len;
     return(d);
}

/*
 * Mail that must never be answered automatically: bounces, auto-replies,
 * mailing-list and bulk mail (RFC 3834).  Replying to these causes mail
 * loops or backscatter.
 */
static BOOL
isautogen(
const CHAR *hdr,
size_t hdrlen,
const IEMAIL *m)
{
     static const CHAR *always[]={"List-Id","List-Unsubscribe","X-Autoreply",
                                  "X-Autorespond","X-Auto-Response-Suppress",
                                  NULL};
     CHAR *v;
     BOOL yes=FALSE;
     INT i;

     if ((v=iemailHeader(hdr,hdrlen,"Auto-Submitted",0)) != NULL) {
          yes=(_strnicmp(v,"no",2) != 0);
          free(v);
     }
     if (!yes && (v=iemailHeader(hdr,hdrlen,"Precedence",0)) != NULL) {
          yes=(_stricmp(v,"bulk") == 0 || _stricmp(v,"junk") == 0
            || _stricmp(v,"list") == 0);
          free(v);
     }
     for (i=0 ; !yes && always[i] != NULL ; i++) {
          if ((v=iemailHeader(hdr,hdrlen,always[i],0)) != NULL) {
               yes=TRUE;
               free(v);
          }
     }
     if (!yes && (v=iemailHeader(hdr,hdrlen,"Return-Path",0)) != NULL) {
          CHAR *s=v;
          while (*s == ' ') s++;
          yes=(strcmp(s,"<>") == 0);
          free(v);
     }
     if (!yes) {
          yes=(_strnicmp(m->from,"mailer-daemon@",14) == 0
            || _strnicmp(m->from,"postmaster@",11) == 0);
     }
     return(yes);
}

/*
 * Who the message is for; see the comment at the top of this file.
 */
static VOID
route(
const CHAR *hdr,
size_t hdrlen,
IEMAIL *m)
{
     static const CHAR *envelope[]={"X-Original-To","Envelope-To",
                                    "X-Envelope-To",NULL};
     CHAR *v,adr[IE_ADRSZ];
     CHAR list[IE_MAXRCPT][IE_ADRSZ];
     INT i,j,n;

     /* 1. the mail host's own (topmost) Received: header */
     if ((v=iemailHeader(hdr,hdrlen,"Received",0)) != NULL) {
          if (iemailReceivedFor(v,adr,sizeof(adr))) {
               addrcpt(m,adr);
               m->rcptsrc=IERS_RECEIVED;
          }
          free(v);
          if (m->nrcpt > 0) {
               return;
          }
     }

     /* 2. envelope headers some hosts add */
     for (i=0 ; envelope[i] != NULL ; i++) {
          if ((v=iemailHeader(hdr,hdrlen,envelope[i],0)) != NULL) {
               n=iemailAddrList(v,list,IE_MAXRCPT,NULL,0);
               for (j=0 ; j < n ; j++) {
                    addrcpt(m,list[j]);
               }
               free(v);
               if (m->nrcpt > 0) {
                    m->rcptsrc=IERS_ENVELOPE;
                    return;
               }
          }
     }

     /* 3. last resort: everyone on To: and Cc: */
     for (i=0 ; i < 2 ; i++) {
          INT k;
          for (k=0 ; (v=iemailHeader(hdr,hdrlen,i == 0 ? "To" : "Cc",k)) != NULL ; k++) {
               n=iemailAddrList(v,list,IE_MAXRCPT,NULL,0);
               for (j=0 ; j < n ; j++) {
                    addrcpt(m,list[j]);
               }
               free(v);
          }
     }
     m->rcptsrc=(m->nrcpt > 0) ? IERS_HEADERS : IERS_NONE;
}

/* add a recipient once (lower case) */
static VOID
addrcpt(
IEMAIL *m,
const CHAR *addr)
{
     CHAR a[IE_ADRSZ];
     INT i;

     sbStrCopy(a,addr,sizeof(a));
     lower(a);
     for (i=0 ; i < m->nrcpt ; i++) {
          if (strcmp(m->rcpt[i],a) == 0) {
               return;
          }
     }
     if (m->nrcpt < IE_MAXRCPT) {
          sbStrCopy(m->rcpt[m->nrcpt++],a,IE_ADRSZ);
     }
}

static VOID
trim(
CHAR *s)
{
     size_t n=strlen(s),i=0;

     while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) {
          s[--n]='\0';
     }
     while (s[i] == ' ' || s[i] == '\t') {
          i++;
     }
     if (i > 0) {
          memmove(s,&s[i],n-i+1);
     }
}

static VOID
lower(
CHAR *s)
{
     for ( ; *s != '\0' ; s++) {
          *s=(CHAR)tolower((UCHAR)*s);
     }
}
