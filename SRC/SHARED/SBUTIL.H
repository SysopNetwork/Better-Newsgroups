/***************************************************************************
 *                                                                         *
 *   SBUTIL.H                                                              *
 *                                                                         *
 *   Better SMTP Relay (SNBSMTPR) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   Small self-contained helpers: Base64, HMAC-MD5 (for CRAM-MD5 login),  *
 *   and bounded string copying.  No BBS SDK calls, so these are safe on   *
 *   the delivery worker thread and in the test programs.                  *
 *                                                                         *
 ***************************************************************************/

#ifndef SBUTIL_H
#define SBUTIL_H

#include <windows.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t                             /*   chars needed incl. terminating NUL */
sbB64Size(                         /* room needed to Base64-encode n bytes */
size_t n);                         /*   number of input bytes              */

size_t                             /*   length of the encoded text         */
sbB64Encode(                       /* Base64-encode a block (no line breaks)*/
const VOID *src,                   /*   bytes to encode                    */
size_t n,                          /*   number of bytes                    */
CHAR *dst);                        /*   output, at least sbB64Size(n) chars*/

INT                                /*   decoded length, or -1 if invalid   */
sbB64Decode(                       /* decode Base64 text                   */
const CHAR *src,                   /*   Base64 text (NUL-terminated)       */
VOID *dst,                         /*   output buffer                      */
INT max);                          /*   size of the output buffer          */

BOOL                               /*   TRUE on success                    */
sbHmacMd5Hex(                      /* HMAC-MD5 as 32 lowercase hex digits  */
const VOID *key,                   /*   key bytes                          */
INT keylen,                        /*   key length                         */
const VOID *msg,                   /*   message bytes                      */
INT msglen,                        /*   message length                     */
CHAR *hex);                        /*   output, at least 33 chars          */

CHAR *                             /*   returns dst                        */
sbStrCopy(                         /* bounded copy, always NUL-terminated  */
CHAR *dst,                         /*   destination                        */
const CHAR *src,                   /*   source (NULL is treated as "")     */
size_t dstsz);                     /*   size of destination                */

CHAR *                             /*   returns dst                        */
sbStrCat(                          /* bounded append, always NUL-terminated*/
CHAR *dst,                         /*   destination                        */
const CHAR *src,                   /*   text to append                     */
size_t dstsz);                     /*   size of destination                */

ULONG                              /*   the CRC                            */
sbCrc32(                           /* CRC-32 as used by ZIP (IEEE 802.3)   */
const VOID *data,                  /*   bytes                              */
size_t n);                         /*   number of bytes                    */

CHAR *                             /*   malloc'd ZIP file, NULL if no memory*/
sbZipStore(                        /* a one-file ZIP, stored uncompressed  */
const CHAR *name,                  /*   file name inside the ZIP (CP437)   */
const VOID *data,                  /*   file contents                      */
size_t n,                          /*   file length                        */
time_t when,                       /*   file date and time                 */
size_t *zlen);                     /*   receives the ZIP's length          */

CHAR *                             /*   malloc'd ZIP file, NULL if no memory*/
sbZipStoreMany(                    /* a ZIP of several files, uncompressed */
INT nfiles,                        /*   how many files (1-65535)           */
const CHAR * const *names,         /*   names inside the ZIP (CP437)       */
const VOID * const *datas,         /*   each file's contents               */
const size_t *lens,                /*   each file's length                 */
time_t when,                       /*   date and time for all files        */
size_t *zlen);                     /*   receives the ZIP's length          */

#ifdef __cplusplus
}
#endif

#endif /* SBUTIL_H */
