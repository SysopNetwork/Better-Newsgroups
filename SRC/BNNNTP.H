/***************************************************************************
 *                                                                         *
 *   BNNNTP.H                                                              *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   NNTP client (RFC 3977, AUTHINFO USER/PASS from RFC 4643, STARTTLS     *
 *   from RFC 4642).  Just the commands a news gateway needs:              *
 *                                                                         *
 *     connect + greeting, MODE READER, STARTTLS, AUTHINFO,                *
 *     GROUP, ARTICLE <number>, POST, QUIT                                 *
 *                                                                         *
 *   Every read goes through one line buffer kept here, so a multi-line   *
 *   reply (an article) is received byte for byte, however long its lines  *
 *   are.  Dot-stuffing is undone on receive and applied on send.          *
 *                                                                         *
 *   Worker-thread only: no BBS SDK calls are made from this code, so it   *
 *   also builds into the BNFETCH tool and the BNTEST unit tests.          *
 *                                                                         *
 ***************************************************************************/

#ifndef BNNNTP_H
#define BNNNTP_H

#include "SBNET.H"                 /* first: winsock2 before windows.h */

#ifdef __cplusplus
extern "C" {
#endif

#define BNSEC_NONE      0          /* plain TCP (usually port 119)         */
#define BNSEC_STARTTLS  1          /* plain connect, then encrypt (119)    */
#define BNSEC_TLS       2          /* encrypted from the start (563)       */

#define BNN_LINESZ      1024       /* one reply line (RFC 3977: 512)       */
#define BNN_RBUFSZ      8192       /* receive buffer                       */

typedef __int64 BNARTNUM;          /* an article number (RFC 3977: up to   */
                                   /* 2^63-1, in practice 32 bits)         */

typedef VOID (*BNLOGFN)(           /* transcript callback                  */
VOID *ctx,                         /*   caller's context                   */
const CHAR *line);                 /*   one line of the conversation       */

typedef struct bnnntp {            /* one NNTP session                     */
     SBNET net;                    /*   the connection (TCP, maybe TLS)    */
     BOOL canpost;                 /*   greeting said posting is allowed   */
     INT code;                     /*   last reply code (-1 = no reply)    */
     CHAR reply[BNN_LINESZ];       /*   last reply line                    */
     CHAR err[SBNET_ERRSZ];        /*   last failure, in plain English     */
     BNLOGFN log;                  /*   transcript (NULL = none)           */
     VOID *logctx;
     CHAR rbuf[BNN_RBUFSZ];        /*   bytes received, not yet consumed   */
     INT rpos,rlen;
} BNNNTP;

VOID
bnnInit(                           /* prepare a session structure          */
BNNNTP *s,
BNLOGFN log,                       /*   transcript callback (or NULL)      */
VOID *logctx);

BOOL                               /*   TRUE when ready for commands       */
bnnConnect(                        /* connect, read greeting, MODE READER  */
BNNNTP *s,
const CHAR *host,                  /*   server name or address             */
INT port,                          /*   TCP port                           */
INT security,                      /*   BNSEC_xxx                          */
BOOL verify,                       /*   check the server certificate (TLS) */
INT timeoutsec);                   /*   per-step timeout                   */

BOOL                               /*   TRUE if logged in                  */
bnnAuth(                           /* AUTHINFO USER / AUTHINFO PASS        */
BNNNTP *s,
const CHAR *user,
const CHAR *pass);

BOOL                               /*   TRUE if the group was selected     */
bnnGroup(                          /* GROUP name                           */
BNNNTP *s,
const CHAR *name,
BNARTNUM *count,                   /*   receives estimated article count   */
BNARTNUM *first,                   /*   receives lowest article number     */
BNARTNUM *last);                   /*   receives highest article number    */

INT                                /*   220 = got it, 423/430 = no such    */
bnnArticle(                        /*   article, other = refused, -1 =     */
BNNNTP *s,                         /*   connection lost                    */
BNARTNUM num,                      /* ARTICLE number                       */
CHAR **buf,                        /*   receives malloc'd article (CRLF)   */
size_t *len,                       /*   receives its length                */
size_t maxlen,                     /*   keep at most this many bytes...    */
BOOL *truncated);                  /*   ...and say so (rest is discarded)  */

INT                                /*   final reply code, -1 = connection  */
bnnPost(                           /* POST an article                      */
BNNNTP *s,
const CHAR *art,                   /*   complete article, CRLF lines, not  */
size_t len);                       /*   dot-stuffed                        */

VOID
bnnQuit(                           /* QUIT (polite end) and close          */
BNNNTP *s);

VOID
bnnClose(                          /* close without QUIT                   */
BNNNTP *s);

/*--- helpers, exposed for the unit tests --------------------------------*/

BOOL                               /*   TRUE if the line is a 211 reply    */
bnnParseGroup(                     /* "211 count first last name"          */
const CHAR *reply,
BNARTNUM *count,
BNARTNUM *first,
BNARTNUM *last);

CHAR *                             /*   malloc'd stuffed copy, NULL = no mem*/
bnnDotStuff(                       /* prepare an article for sending:      */
const CHAR *art,                   /*   CRLF line ends throughout, '.'     */
size_t len,                        /*   doubled at line starts, ".\r\n"    */
size_t *outlen);                   /*   terminator added                   */

#ifdef __cplusplus
}
#endif

#endif /* BNNNTP_H */
