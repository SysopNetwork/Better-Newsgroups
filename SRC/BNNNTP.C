/***************************************************************************
 *                                                                         *
 *   BNNNTP.C                                                              *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   NNTP client.  A news server conversation is a series of one-line     *
 *   commands, each answered by a line starting with a 3-digit code:       *
 *                                                                         *
 *     2xx  done            3xx  go on (send the rest)                     *
 *     4xx  not now / no    5xx  not supported / not allowed               *
 *                                                                         *
 *   Some answers (an article) are followed by many lines, ended by a line *
 *   holding a single ".".  A text line that itself starts with "." has   *
 *   that dot doubled on the wire ("dot-stuffing"), both ways.             *
 *                                                                         *
 *   Worker-thread only: no BBS SDK calls are made from this file.         *
 *                                                                         *
 ***************************************************************************/

#include "BNNNTP.H"
#include "SBUTIL.H"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define ARTLINESZ  16384           /* longest article line kept whole      */

static INT getbyte(BNNNTP *s);
static INT readline(BNNNTP *s, CHAR *buf, INT bufsz);
static INT readreply(BNNNTP *s);
static BOOL sendcmd(BNNNTP *s, const CHAR *fmt, ...);
static VOID logf(BNNNTP *s, const CHAR *fmt, ...);
static VOID seterr(BNNNTP *s, const CHAR *fmt, ...);
static VOID replyerr(BNNNTP *s, const CHAR *what);

VOID
bnnInit(                           /* prepare a session structure          */
BNNNTP *s,
BNLOGFN log,
VOID *logctx)
{
     memset(s,0,sizeof(*s));
     sbnetInit(&s->net);
     s->code=-1;
     s->log=log;
     s->logctx=logctx;
}

BOOL                               /*   TRUE when ready for commands       */
bnnConnect(                        /* connect, read greeting, MODE READER  */
BNNNTP *s,
const CHAR *host,
INT port,
INT security,
BOOL verify,
INT timeoutsec)
{
     logf(s,"Connecting to %s port %d%s",host,port,
          security == BNSEC_TLS ? " (TLS)"
          : security == BNSEC_STARTTLS ? " (STARTTLS)" : "");
     if (!sbnetConnect(&s->net,host,port,timeoutsec)) {
          seterr(s,"%s",s->net.err);
          return(FALSE);
     }
     logf(s,"Connected to %s",s->net.peer);
     if (security == BNSEC_TLS) {
          if (!sbnetStartTLS(&s->net,verify)) {
               seterr(s,"%s",s->net.err);
               return(FALSE);
          }
          logf(s,"Encrypted with %s",s->net.tlsinfo);
     }

     /* greeting: 200 = posting allowed, 201 = reading only */
     if (readreply(s) < 0) {
          return(FALSE);
     }
     if (s->code != 200 && s->code != 201) {
          replyerr(s,"The news server refused the connection");
          return(FALSE);
     }
     s->canpost=(s->code == 200);

     if (security == BNSEC_STARTTLS) {
          if (!sendcmd(s,"STARTTLS") || readreply(s) < 0) {
               return(FALSE);
          }
          if (s->code != 382) {
               replyerr(s,"The news server does not offer STARTTLS");
               return(FALSE);
          }
          /*
           * Nothing may sit unread between the server's "go ahead" and
           * the TLS handshake: anything there would be an attempt to slip
           * plain-text replies into the encrypted session.
           */
          if (s->rpos < s->rlen) {
               seterr(s,"Server sent unexpected data before TLS negotiation");
               return(FALSE);
          }
          if (!sbnetStartTLS(&s->net,verify)) {
               seterr(s,"%s",s->net.err);
               return(FALSE);
          }
          logf(s,"Encrypted with %s",s->net.tlsinfo);
     }

     /*
      * MODE READER asks a server that also takes feeds from other servers
      * to act as a reading server.  Servers that are reading servers
      * already may answer with an error; that is harmless, so any reply
      * (other than a dropped connection) lets us go on.
      */
     if (!sendcmd(s,"MODE READER") || readreply(s) < 0) {
          return(FALSE);
     }
     if (s->code == 200) {
          s->canpost=TRUE;
     }
     else if (s->code == 201) {
          s->canpost=FALSE;
     }
     return(TRUE);
}

BOOL                               /*   TRUE if logged in                  */
bnnAuth(                           /* AUTHINFO USER / AUTHINFO PASS        */
BNNNTP *s,
const CHAR *user,
const CHAR *pass)
{
     if (user == NULL || user[0] == '\0') {
          return(TRUE);            /* no login configured                 */
     }
     if (!sendcmd(s,"AUTHINFO USER %s",user) || readreply(s) < 0) {
          return(FALSE);
     }
     if (s->code == 381) {         /* "password required"                 */
          if (!sendcmd(s,"AUTHINFO PASS %s",pass != NULL ? pass : "")
           || readreply(s) < 0) {
               return(FALSE);
          }
     }
     if (s->code != 281) {
          replyerr(s,s->code == 481 ? "Login refused (check the login name "
                   "and password)" : "Login failed");
          return(FALSE);
     }
     /* posting permission is often granted only once logged in */
     s->canpost=TRUE;
     return(TRUE);
}

BOOL                               /*   TRUE if the group was selected     */
bnnGroup(                          /* GROUP name                           */
BNNNTP *s,
const CHAR *name,
BNARTNUM *count,
BNARTNUM *first,
BNARTNUM *last)
{
     if (!sendcmd(s,"GROUP %s",name) || readreply(s) < 0) {
          return(FALSE);
     }
     if (s->code == 211 && bnnParseGroup(s->reply,count,first,last)) {
          return(TRUE);
     }
     replyerr(s,s->code == 411 ? "No such newsgroup on this server"
              : s->code == 480 ? "The server wants a login first"
              : "Could not open the newsgroup");
     return(FALSE);
}

INT                                /*   220 = got it, 423/430 = no such    */
bnnArticle(                        /*   article, other = refused, -1 =     */
BNNNTP *s,                         /*   connection lost                    */
BNARTNUM num,
CHAR **buf,
size_t *len,
size_t maxlen,
BOOL *truncated)
{
     CHAR *art,*line;
     size_t used=0,room;
     INT n;

     *buf=NULL;
     *len=0;
     *truncated=FALSE;
     if (!sendcmd(s,"ARTICLE %I64d",num) || readreply(s) < 0) {
          return(-1);
     }
     if (s->code != 220) {
          return(s->code);
     }

     room=(maxlen < 65536) ? maxlen+3 : 65536;
     art=(CHAR *)malloc(room);
     line=(CHAR *)malloc(ARTLINESZ);
     if (art == NULL || line == NULL) {
          free(art);
          free(line);
          seterr(s,"Out of memory");
          bnnClose(s);             /* the rest of the reply is unread     */
          return(-1);
     }

     /*
      * Read lines until the lone "."; undo dot-stuffing; keep up to
      * maxlen bytes.  Past the limit the lines are still read (to stay in
      * step with the server) but not kept.
      */
     for (;;) {
          const CHAR *p;
          size_t ll;

          if ((n=readline(s,line,ARTLINESZ)) < 0) {
               free(art);
               free(line);
               return(-1);
          }
          if (n == 1 && line[0] == '.') {
               break;
          }
          p=line;
          ll=(size_t)n;
          if (line[0] == '.') {
               p++;                /* ".." at line start stands for "."   */
               ll--;
          }
          if (*truncated || used+ll+2 > maxlen) {
               *truncated=TRUE;
               continue;
          }
          if (used+ll+3 > room) {
               size_t want=room*2;
               CHAR *bigger;

               while (want < used+ll+3) {
                    want*=2;
               }
               if (want > maxlen+3) {
                    want=maxlen+3;
               }
               if ((bigger=(CHAR *)realloc(art,want)) == NULL) {
                    *truncated=TRUE;
                    continue;
               }
               art=bigger;
               room=want;
          }
          memcpy(art+used,p,ll);
          used+=ll;
          art[used++]='\r';
          art[used++]='\n';
     }
     free(line);
     art[used]='\0';
     *buf=art;
     *len=used;
     if (s->log != NULL) {
          logf(s,"   (article %I64d: %u bytes%s)",num,(unsigned)used,
               *truncated ? ", truncated" : "");
     }
     return(220);
}

INT                                /*   final reply code, -1 = connection  */
bnnPost(                           /* POST an article                      */
BNNNTP *s,
const CHAR *art,
size_t len)
{
     CHAR *stuffed;
     size_t slen;
     BOOL ok;

     if (!sendcmd(s,"POST") || readreply(s) < 0) {
          return(-1);
     }
     if (s->code != 340) {
          return(s->code);         /* 440: posting not permitted          */
     }
     if ((stuffed=bnnDotStuff(art,len,&slen)) == NULL) {
          seterr(s,"Out of memory");
          bnnClose(s);
          return(-1);
     }
     if (s->log != NULL) {
          logf(s,"   (sending article, %u bytes)",(unsigned)len);
     }
     ok=sbnetSend(&s->net,stuffed,(INT)slen);
     free(stuffed);
     if (!ok) {
          seterr(s,"%s",s->net.err);
          return(-1);
     }
     if (readreply(s) < 0) {
          return(-1);
     }
     return(s->code);              /* 240: posted; 441: refused           */
}

VOID
bnnQuit(                           /* QUIT (polite end) and close          */
BNNNTP *s)
{
     if (s->net.skt != INVALID_SOCKET) {
          if (sendcmd(s,"QUIT")) {
               readreply(s);
          }
     }
     bnnClose(s);
}

VOID
bnnClose(                          /* close without QUIT                   */
BNNNTP *s)
{
     sbnetClose(&s->net);
     s->rpos=s->rlen=0;
}

/*--- helpers ------------------------------------------------------------*/

BOOL                               /*   TRUE if the line is a 211 reply    */
bnnParseGroup(                     /* "211 count first last name"          */
const CHAR *reply,
BNARTNUM *count,
BNARTNUM *first,
BNARTNUM *last)
{
     BNARTNUM c,f,l;

     if (sscanf(reply,"211 %I64d %I64d %I64d",&c,&f,&l) != 3) {
          return(FALSE);
     }
     /*
      * An empty group may be reported as "211 0 0 0", or with last one
      * lower than first; both mean "nothing to read".
      */
     *count=c;
     *first=f;
     *last=l;
     return(TRUE);
}

CHAR *                             /*   malloc'd stuffed copy, NULL = no mem*/
bnnDotStuff(                       /* prepare an article for sending       */
const CHAR *art,
size_t len,
size_t *outlen)
{
     CHAR *out;
     size_t i,o=0,lines=1;
     BOOL atstart=TRUE;

     /* worst case: every line gains a dot and a CR, plus the terminator */
     for (i=0 ; i < len ; i++) {
          if (art[i] == '\n') {
               lines++;
          }
     }
     if ((out=(CHAR *)malloc(len+lines*2+8)) == NULL) {
          return(NULL);
     }
     for (i=0 ; i < len ; i++) {
          CHAR c=art[i];

          if (atstart && c == '.') {
               out[o++]='.';       /* a leading dot is sent doubled       */
          }
          atstart=FALSE;
          if (c == '\r') {
               continue;           /* line ends are rebuilt as CRLF below */
          }
          if (c == '\n') {
               out[o++]='\r';
               out[o++]='\n';
               atstart=TRUE;
               continue;
          }
          out[o++]=c;
     }
     if (!atstart) {               /* last line had no line end           */
          out[o++]='\r';
          out[o++]='\n';
     }
     out[o++]='.';
     out[o++]='\r';
     out[o++]='\n';
     out[o]='\0';
     *outlen=o;
     return(out);
}

/*--- reading and writing ------------------------------------------------*/

static INT                         /*   byte, or -1 on error / closed      */
getbyte(
BNNNTP *s)
{
     if (s->rpos >= s->rlen) {
          INT got=sbnetRead(&s->net,s->rbuf,BNN_RBUFSZ);

          if (got <= 0) {
               seterr(s,"%s",s->net.err[0] != '\0' ? s->net.err
                      : "Server closed the connection");
               return(-1);
          }
          s->rpos=0;
          s->rlen=got;
     }
     return((UCHAR)s->rbuf[s->rpos++]);
}

/*
 * One line, CR/LF removed.  A line longer than the buffer is cut short
 * (the rest is read and dropped) so a misbehaving server cannot overrun
 * anything.  Returns the length kept, or -1 if the connection failed.
 */
static INT
readline(
BNNNTP *s,
CHAR *buf,
INT bufsz)
{
     INT len=0,c;

     for (;;) {
          if ((c=getbyte(s)) < 0) {
               return(-1);
          }
          if (c == '\n') {
               break;
          }
          if (len < bufsz-1) {
               buf[len++]=(CHAR)c;
          }
     }
     if (len > 0 && buf[len-1] == '\r') {
          len--;
     }
     buf[len]='\0';
     return(len);
}

static INT                         /*   reply code, or -1 on failure       */
readreply(
BNNNTP *s)
{
     s->code=-1;
     if (readline(s,s->reply,sizeof(s->reply)) < 0) {
          return(-1);
     }
     logf(s,"S: %s",s->reply);
     if (s->reply[0] < '1' || s->reply[0] > '5'
      || s->reply[1] < '0' || s->reply[1] > '9'
      || s->reply[2] < '0' || s->reply[2] > '9') {
          seterr(s,"Not a news server reply: %.80s",s->reply);
          bnnClose(s);
          return(-1);
     }
     s->code=atoi(s->reply);
     return(s->code);
}

static BOOL
sendcmd(                           /* send one command line                */
BNNNTP *s,
const CHAR *fmt,
...)
{
     CHAR line[BNN_LINESZ];
     va_list ap;
     INT n;

     va_start(ap,fmt);
     n=_vsnprintf(line,sizeof(line)-3,fmt,ap);
     va_end(ap);
     if (n < 0) {
          seterr(s,"Command too long");
          return(FALSE);
     }
     line[sizeof(line)-3]='\0';
     /* never put a password in the transcript */
     if (_strnicmp(line,"AUTHINFO PASS",13) == 0) {
          logf(s,"C: AUTHINFO PASS ********");
     }
     else {
          logf(s,"C: %s",line);
     }
     n=(INT)strlen(line);
     line[n++]='\r';
     line[n++]='\n';
     if (!sbnetSend(&s->net,line,n)) {
          seterr(s,"%s",s->net.err);
          return(FALSE);
     }
     return(TRUE);
}

static VOID
logf(                              /* one transcript line                  */
BNNNTP *s,
const CHAR *fmt,
...)
{
     CHAR line[BNN_LINESZ+32];
     va_list ap;

     if (s->log == NULL) {
          return;
     }
     va_start(ap,fmt);
     _vsnprintf(line,sizeof(line)-1,fmt,ap);
     va_end(ap);
     line[sizeof(line)-1]='\0';
     (*s->log)(s->logctx,line);
}

static VOID
seterr(
BNNNTP *s,
const CHAR *fmt,
...)
{
     va_list ap;

     va_start(ap,fmt);
     _vsnprintf(s->err,sizeof(s->err)-1,fmt,ap);
     va_end(ap);
     s->err[sizeof(s->err)-1]='\0';
     logf(s,"*** %s",s->err);
}

static VOID
replyerr(                          /* error text that quotes the reply     */
BNNNTP *s,
const CHAR *what)
{
     seterr(s,"%s: %.120s",what,s->reply);
}
