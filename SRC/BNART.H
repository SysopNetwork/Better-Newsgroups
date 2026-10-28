/***************************************************************************
 *                                                                         *
 *   BNART.H                                                               *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   Usenet articles (RFC 5536) in and out:                                *
 *                                                                         *
 *     out: a BBS forum message (CP437 text, ANSI colours, CR line ends)  *
 *          becomes a plain UTF-8 article with proper headers.             *
 *     in:  the pieces of an incoming article's header that a news         *
 *          gateway needs beyond what IEMSG.C already decodes: the thread  *
 *          references and the date.                                       *
 *                                                                         *
 *   No BBS SDK calls: used by the main thread, the worker thread and the  *
 *   test programs.                                                        *
 *                                                                         *
 ***************************************************************************/

#ifndef BNART_H
#define BNART_H

#include <windows.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BN_MIDSZ        248        /* a Message-ID without < > (+ NUL)     */
#define BN_MAXPAR       3          /* parent references kept per article   */
#define BN_LINEMAX      998        /* longest line an article may carry    */

typedef struct bnpost {            /* everything an outgoing article needs */
     CHAR newsgroups[512];         /*   "group" or "group1,group2"         */
     CHAR fromname[128];           /*   display name, CP437 ("" = none)    */
     CHAR fromadr[256];            /*   address                            */
     CHAR subject[128];            /*   CP437                              */
     CHAR date[64];                /*   RFC 5322 date                      */
     CHAR msgid[BN_MIDSZ];         /*   without < >                        */
     CHAR root[BN_MIDSZ];          /*   first reference (thread start), "" */
     CHAR parent[BN_MIDSZ];        /*   article this replies to, ""        */
     CHAR org[128];                /*   Organization, CP437 ("" = none)    */
     CHAR useragent[96];           /*   User-Agent header value            */
} BNPOST;

CHAR *                             /*   malloc'd article, NULL = no memory */
bnartBuild(                        /* build an outgoing article            */
const BNPOST *p,                   /*   headers                            */
const CHAR *body,                  /*   BBS message text (CP437, may hold  */
                                   /*   ANSI codes, CR or LF line ends)    */
const CHAR *sig,                   /*   signature (CP437, "" = none)       */
size_t *len);                      /*   receives the article's length      */

VOID
bnartEncodeWord(                   /* header text: CP437 to UTF-8, as an   */
const CHAR *cp437,                 /*   RFC 2047 encoded word only when it */
CHAR *dst,                         /*   is not plain ASCII                 */
size_t dstsz);

INT                                /*   number of parents found            */
bnartRefs(                         /* thread references of an article      */
const CHAR *hdr,                   /*   header block                       */
size_t hlen,                       /*   its length                         */
CHAR *root,                        /*   receives the first reference ("") */
CHAR (*parents)[BN_MIDSZ]);        /*   receives up to BN_MAXPAR parents,  */
                                   /*   the direct parent first            */

BOOL                               /*   TRUE if the date could be read     */
bnartParseDate(                    /* RFC 5322 date to a UTC time          */
const CHAR *date,                  /*   e.g. "Mon, 20 Oct 2026 01:23:45    */
time_t *utc);                      /*         -0400"                        */

VOID
bnartMakeMsgId(                    /* a new Message-ID (without < >)       */
CHAR *dst,
size_t dstsz,
ULONG a,                           /*   two numbers that make it unique on */
ULONG b,                           /*   this system (GME message id parts) */
const CHAR *host);                 /*   our host name                      */

#ifdef __cplusplus
}
#endif

#endif /* BNART_H */
