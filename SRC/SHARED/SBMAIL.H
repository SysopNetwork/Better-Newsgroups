/***************************************************************************
 *                                                                         *
 *   SBMAIL.H                                                              *
 *                                                                         *
 *   Better SMTP Relay (SNBSMTPR) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   One outgoing e-mail, described as parts (sender, recipient, subject, *
 *   body text file, optional attachment), plus the helpers that turn BBS *
 *   text into Internet mail:                                              *
 *     - CP437 (the BBS character set) to UTF-8                            *
 *     - ANSI colour codes removed, line endings made CRLF                 *
 *     - building the final MIME message for SMTP delivery                 *
 *                                                                         *
 *   Both delivery methods start from the same SBMAIL: SMTP sends the     *
 *   MIME message built by sbmailBuildMime(), the HTTP API sends the parts *
 *   as JSON fields.                                                       *
 *                                                                         *
 *   No BBS SDK calls: usable on the main thread and the worker thread.   *
 *                                                                         *
 ***************************************************************************/

#ifndef SBMAIL_H
#define SBMAIL_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBM_ADRSZ    256           /* e-mail address                       */
#define SBM_NAMESZ   128           /* display name (UTF-8)                 */
#define SBM_SUBJSZ   512           /* subject (UTF-8, up to 3x CP437 size) */
#define SBM_DATESZ   64            /* RFC 5322 date                        */
#define SBM_IDSZ     160           /* Message-ID                           */
#define SBM_FILESZ   MAX_PATH      /* file path                            */
#define SBM_ATTNSZ   128           /* attachment file name shown to reader */

typedef struct sbmail {            /* one outgoing message                 */
     CHAR envfrom[SBM_ADRSZ];      /*   envelope sender (MAIL FROM)        */
     CHAR fromname[SBM_NAMESZ];    /*   From: display name (may be "")     */
     CHAR fromadr[SBM_ADRSZ];      /*   From: address                      */
     CHAR replyto[SBM_ADRSZ];      /*   Reply-To: address (may be "")      */
     CHAR to[SBM_ADRSZ];           /*   recipient                          */
     CHAR subject[SBM_SUBJSZ];     /*   subject, UTF-8                     */
     CHAR date[SBM_DATESZ];        /*   Date: header value                 */
     CHAR msgid[SBM_IDSZ];         /*   Message-ID: value, with <>         */
     CHAR bodyfile[SBM_FILESZ];    /*   body text file, CRLF lines         */
     BOOL cp437body;               /*   body file is CP437 (else UTF-8)    */
     CHAR attfile[SBM_FILESZ];     /*   attachment file ("" = none)        */
     CHAR attname[SBM_ATTNSZ];     /*   attachment name for the reader     */
} SBMAIL;

/* --- text conversion (BBS text to Internet text) ---------------------- */

size_t                             /*   bytes written (excluding NUL)      */
sbCp437ToUtf8(                     /* convert CP437 text to UTF-8          */
const CHAR *src,                   /*   CP437 text (NUL-terminated)        */
CHAR *dst,                         /*   output buffer                      */
size_t dstsz);                     /*   size of output (3x input is safe)  */

size_t                             /*   bytes written (excluding NUL)      */
sbCleanBbsText(                    /* strip ANSI codes, normalize to CRLF  */
const CHAR *src,                   /*   BBS message text (CR or LF lines)  */
CHAR *dst,                         /*   output buffer                      */
size_t dstsz);                     /*   size (2x input + 3 is always safe) */

VOID
sbRfc5322Date(                     /* "Mon, 12 Oct 2026 10:00:00 -0500"    */
CHAR *dst,                         /*   output, at least SBM_DATESZ        */
size_t dstsz);

VOID
sbMakeMsgId(                       /* "<unique@host>"                      */
CHAR *dst,                         /*   output, at least SBM_IDSZ          */
size_t dstsz,
const CHAR *host);                 /*   our host name                      */

const CHAR *                       /*   e.g. "application/pdf"             */
sbMimeType(                        /* content type for a file name         */
const CHAR *fname);

/* --- building the message --------------------------------------------- */

CHAR *                             /*   malloc'd message, NULL on failure  */
sbmailBuildMime(                   /* full RFC 5322 / MIME message (CRLF)  */
const SBMAIL *m,                   /*   message parts                      */
LONG *len,                         /*   receives the message length        */
CHAR *err,                         /*   receives a reason on failure       */
size_t errsz);

CHAR *                             /*   malloc'd UTF-8 text, NULL = failed */
sbmailReadBody(                    /* read the body file, as UTF-8         */
const SBMAIL *m,
LONG *len,                         /*   receives the length                */
LONG maxlen);                      /*   refuse files larger than this      */

CHAR *                             /*   malloc'd file contents or NULL     */
sbReadFile(                        /* read a whole file into memory        */
const CHAR *path,
LONG *len,                         /*   receives the length                */
LONG maxlen);                      /*   refuse files larger than this      */

#ifdef __cplusplus
}
#endif

#endif /* SBMAIL_H */
