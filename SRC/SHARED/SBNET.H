/***************************************************************************
 *                                                                         *
 *   SBNET.H                                                               *
 *                                                                         *
 *   Better SMTP Relay (SNBSMTPR) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   Blocking TCP connection with optional TLS, built on Winsock and the   *
 *   Windows Schannel security provider.                                   *
 *                                                                         *
 *   This code runs ONLY on the delivery worker thread (or in the command  *
 *   line test tool).  It makes no BBS SDK calls and reads no BBS globals, *
 *   because blocking network I/O on the BBS main thread would freeze      *
 *   every user on the system.                                             *
 *                                                                         *
 ***************************************************************************/

#ifndef SBNET_H
#define SBNET_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBNET_ERRSZ    256         /* room for a readable error message    */
#define SBNET_INFOSZ   64          /* room for "TLS 1.2" style descriptions*/

typedef struct sbnet {             /* one network connection               */
     SOCKET skt;                   /*   connected socket (or INVALID)      */
     INT timeout;                  /*   per-operation timeout, milliseconds*/
     BOOL tls;                     /*   TRUE once TLS is established       */
     BOOL havecred;                /*   Schannel credentials acquired?     */
     BOOL havectx;                 /*   Schannel security context exists?  */
     CredHandle cred;              /*   Schannel credentials handle        */
     CtxtHandle ctx;               /*   Schannel security context          */
     SecPkgContext_StreamSizes sizes; /* TLS record header/trailer sizes   */
     CHAR *raw;                    /*   received bytes not yet decrypted   */
     INT rawlen;                   /*   bytes waiting in raw               */
     INT rawsz;                    /*   capacity of raw                    */
     CHAR *plain;                  /*   received plaintext not yet consumed*/
     INT plainpos;                 /*   read position in plain             */
     INT plainlen;                 /*   bytes of plaintext in plain        */
     INT plainsz;                  /*   capacity of plain                  */
     CHAR *sndbuf;                 /*   scratch buffer for encrypting      */
     CHAR host[256];               /*   server name (TLS SNI + cert check) */
     CHAR peer[64];                /*   numeric address we connected to    */
     CHAR tlsinfo[SBNET_INFOSZ];   /*   negotiated protocol, e.g. "TLS 1.2"*/
     CHAR err[SBNET_ERRSZ];        /*   last error, in plain English       */
} SBNET;

VOID
sbnetInit(                         /* prepare a connection structure       */
SBNET *n);                         /*   connection to initialize           */

BOOL                               /*   returns TRUE if connected          */
sbnetConnect(                      /* resolve and connect to a server      */
SBNET *n,                          /*   connection (initialized)           */
const CHAR *host,                  /*   host name or numeric address       */
INT port,                          /*   TCP port number                    */
INT timeoutsec);                   /*   connect and I/O timeout in seconds */

BOOL                               /*   returns TRUE if TLS is now active  */
sbnetStartTLS(                     /* negotiate TLS on a connected socket  */
SBNET *n,                          /*   connection                         */
BOOL verify);                      /*   verify the server's certificate?   */

BOOL                               /*   returns TRUE if all bytes were sent*/
sbnetSend(                         /* send bytes (encrypted if TLS active) */
SBNET *n,                          /*   connection                         */
const VOID *data,                  /*   bytes to send                      */
INT len);                          /*   number of bytes                    */

INT                                /*   line length, 0 on EOF, -1 on error */
sbnetReadLine(                     /* read one CRLF-terminated line        */
SBNET *n,                          /*   connection                         */
CHAR *line,                        /*   buffer for the line (CRLF removed) */
INT max);                          /*   size of the buffer                 */

INT                                /*   bytes read, 0 on EOF, -1 on error  */
sbnetRead(                         /* read whatever bytes are available,   */
SBNET *n,                          /*   unchanged (no line handling)       */
VOID *buf,                         /*   buffer for the bytes               */
INT max);                          /*   size of the buffer                 */

BOOL                               /*   TRUE if no unread input is buffered*/
sbnetInputEmpty(                   /* check for unread received data       */
SBNET *n);                         /*   connection                         */

VOID
sbnetAbort(                        /* force a blocked connection to fail   */
SBNET *n);                         /*   (safe to call from another thread) */

VOID
sbnetClose(                        /* shut down TLS, close, free buffers   */
SBNET *n);                         /*   connection                         */

#ifdef __cplusplus
}
#endif

#endif /* SBNET_H */
