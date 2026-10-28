/***************************************************************************
 *                                                                         *
 *   IEMSG.H                                                               *
 *                                                                         *
 *   Better Internet Email - mail receiver (SNBIEMLR) for The Major BBS v10*
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   Turns one Internet mail message (RFC 5322 + MIME) into what a BBS    *
 *   message needs: who it is for, who it is from, a subject, plain CP437  *
 *   text, and any attached files.                                         *
 *                                                                         *
 *   IETEXT.C holds the text conversions (character sets to CP437, quoted- *
 *   printable, Base64, encoded subjects, HTML to text, word wrap).        *
 *                                                                         *
 *   Worker-thread only: no BBS SDK calls are made from this code, so it   *
 *   also builds into the IEFETCH tool and the IETEST unit tests.          *
 *                                                                         *
 ***************************************************************************/

#ifndef IEMSG_H
#define IEMSG_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IE_ADRSZ        256        /* an e-mail address (GME MAXADR)       */
#define IE_NAMESZ       128        /* a display name                       */
#define IE_SUBJSZ       256        /* a subject (BBS topic is shorter)     */
#define IE_IDSZ         256        /* a Message-ID                         */
#define IE_MAXRCPT      32         /* recipients kept per message          */
#define IE_MAXATT       32         /* attachments kept per message         */
#define IE_FNAMESZ      128        /* an attachment's original file name   */

                                   /* where the recipients came from       */
#define IERS_NONE       0          /*   nothing usable was found           */
#define IERS_RECEIVED   1          /*   the mail host's "for <addr>" clause*/
#define IERS_ENVELOPE   2          /*   X-Original-To / Envelope-To header */
#define IERS_HEADERS    3          /*   To: and Cc: (may need de-duplicate)*/

typedef struct ieatt {             /* one attached file                    */
     CHAR name[IE_FNAMESZ];        /*   original name, CP437 ("" = none)   */
     CHAR type[64];                /*   MIME type, lower case              */
     CHAR *data;                   /*   decoded bytes (malloc'd)           */
     size_t len;                   /*   number of bytes                    */
} IEATT;

typedef struct iemail {            /* one message, ready for the BBS       */
     CHAR rcpt[IE_MAXRCPT][IE_ADRSZ]; /* who it is for (lower case)        */
     INT nrcpt;                    /*   how many                           */
     INT rcptsrc;                  /*   IERS_xxx                           */
     CHAR from[IE_ADRSZ];          /*   sender's address (From:)           */
     CHAR fromname[IE_NAMESZ];     /*   sender's display name, CP437       */
     CHAR replyto[IE_ADRSZ];       /*   Reply-To: address, if any          */
     CHAR retpath[IE_ADRSZ];       /*   Return-Path (envelope sender)      */
     CHAR subject[IE_SUBJSZ];      /*   subject, decoded to CP437          */
     CHAR date[64];                /*   Date: as sent                      */
     CHAR msgid[IE_IDSZ];          /*   Message-ID, without < >            */
     BOOL autogen;                 /*   bounce/auto-reply/list mail: never */
                                   /*   answer it automatically            */
     BOOL fromhtml;                /*   text was converted from HTML       */
     CHAR *text;                   /*   message text, CP437, '\n' lines    */
     size_t textlen;               /*   length of text                     */
     IEATT att[IE_MAXATT];         /*   attached files                     */
     INT natt;                     /*   how many                           */
     INT dropped;                  /*   attachments beyond IE_MAXATT       */
     CHAR *header;                 /*   the raw header block (malloc'd)    */
} IEMAIL;

/*--- IEMSG.C -------------------------------------------------------------*/

BOOL                               /*   FALSE only when out of memory      */
iemailParse(                       /* decode a whole message               */
const CHAR *raw,                   /*   the message as downloaded (CRLF)   */
size_t len,                        /*   its length                         */
INT wrapcol,                       /*   word-wrap column (0 = no wrap)     */
IEMAIL *m);                        /*   receives the result (iemailFree it)*/

VOID
iemailFree(                        /* release everything iemailParse made  */
IEMAIL *m);

CHAR *                             /*   value (malloc'd, unfolded) or NULL */
iemailHeader(                      /* find a header in a header block      */
const CHAR *hdr,                   /*   header block (CRLF or LF lines)    */
size_t hdrlen,                     /*   its length                         */
const CHAR *name,                  /*   header name, e.g. "Subject"        */
INT which);                        /*   0 = first, 1 = second, ...         */

BOOL                               /*   TRUE if an address was found       */
iemailReceivedFor(                 /* the "for <addr>" clause of a         */
const CHAR *received,              /*   Received: header value             */
CHAR *addr,                        /*   receives the address               */
size_t addrsz);

INT                                /*   number of addresses found          */
iemailAddrList(                    /* split an address header value        */
const CHAR *value,                 /*   e.g. "Ann <a@x>, \"B, C\" <b@x>"   */
CHAR (*addrs)[IE_ADRSZ],           /*   receives the bare addresses        */
INT max,                           /*   room in addrs                      */
CHAR *firstname,                   /*   receives the first display name    */
size_t namesz);                    /*   (raw, may hold encoded words)      */

/*--- IETEXT.C ------------------------------------------------------------*/

CHAR *                             /*   malloc'd CP437 text, NULL = no mem */
ietextToCp437(                     /* convert text in any charset to CP437 */
const CHAR *src,                   /*   the text                           */
size_t len,                        /*   its length                         */
const CHAR *charset,               /*   MIME charset ("" = guess)          */
BOOL html,                         /*   convert HTML to plain text first   */
INT wrapcol,                       /*   word-wrap column (0 = no wrap)     */
size_t *outlen);                   /*   receives the length                */

VOID
ietextDecodeWords(                 /* decode RFC 2047 =?cs?Q/B?...?= words */
const CHAR *src,                   /*   header text                        */
CHAR *dst,                         /*   receives one CP437 line            */
size_t dstsz);

size_t                             /*   decoded length                     */
ietextQPDecode(                    /* quoted-printable to bytes, in place  */
CHAR *buf,                         /*   the encoded text                   */
size_t len);                       /*   its length                         */

size_t                             /*   decoded length                     */
ietextB64Decode(                   /* Base64 to bytes, in place, skipping  */
CHAR *buf,                         /*   line breaks and junk               */
size_t len);

VOID
ietextSafeName(                    /* make a DOS 8.3 file name for the BBS */
const CHAR *name,                  /*   original name (may be "")          */
const CHAR *type,                  /*   MIME type, to pick an extension    */
INT seq,                           /*   1, 2, ... used when name is unusable*/
CHAR *dst);                        /*   receives the name (13 bytes)       */

#ifdef __cplusplus
}
#endif

#endif /* IEMSG_H */
