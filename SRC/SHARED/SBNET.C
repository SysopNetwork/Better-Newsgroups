/***************************************************************************
 *                                                                         *
 *   SBNET.C                                                               *
 *                                                                         *
 *   Better SMTP Relay (SNBSMTPR) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.  Released under the MIT License.    *
 *                                                                         *
 *   Blocking TCP connection with optional TLS (Windows Schannel).         *
 *                                                                         *
 *   Two TLS styles are supported by the same code:                        *
 *     - "implicit" TLS (ports such as 465): sbnetStartTLS() is called     *
 *       immediately after connecting, before any SMTP traffic.            *
 *     - STARTTLS (ports such as 587/2525): the SMTP layer asks the server *
 *       to upgrade, then calls sbnetStartTLS() on the same socket.        *
 *                                                                         *
 *   Worker-thread only: no BBS SDK calls are made from this file.         *
 *                                                                         *
 ***************************************************************************/

#include "SBNET.H"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")

#define PLAINSZ   4096             /* plaintext buffer before TLS starts   */

static VOID seterr(SBNET *n, const CHAR *fmt, ...);
static VOID setwsaerr(SBNET *n, const CHAR *what, INT code);
static VOID setsecerr(SBNET *n, const CHAR *what, SECURITY_STATUS ss);
static BOOL sendraw(SBNET *n, const CHAR *data, INT len);
static INT recvraw(SBNET *n, CHAR *buf, INT len);
static BOOL handshake(SBNET *n, BOOL initial);
static INT fillplain(SBNET *n);
static VOID describetls(SBNET *n);

VOID
sbnetInit(                         /* prepare a connection structure       */
SBNET *n)                          /*   connection to initialize           */
{
     memset(n,0,sizeof(*n));
     n->skt=INVALID_SOCKET;
     n->timeout=60000;
}

BOOL                               /*   returns TRUE if connected          */
sbnetConnect(                      /* resolve and connect to a server      */
SBNET *n,                          /*   connection (initialized)           */
const CHAR *host,                  /*   host name or numeric address       */
INT port,                          /*   TCP port number                    */
INT timeoutsec)                    /*   connect and I/O timeout in seconds */
{
     struct addrinfo hints,*list,*ai;
     CHAR portstr[16];
     INT rc,lasterr;
     DWORD tmo;

     n->timeout=timeoutsec*1000;
     strncpy(n->host,host,sizeof(n->host)-1);
     n->host[sizeof(n->host)-1]='\0';

     /*
      * Plaintext input buffer.  It is replaced by a larger one when TLS
      * starts, because a TLS record can carry up to 16 KB.
      */
     if ((n->plain=(CHAR *)malloc(PLAINSZ)) == NULL) {
          seterr(n,"Out of memory");
          return(FALSE);
     }
     n->plainsz=PLAINSZ;

     /* look up every address for the host (IPv4 and IPv6) */
     memset(&hints,0,sizeof(hints));
     hints.ai_family=AF_UNSPEC;
     hints.ai_socktype=SOCK_STREAM;
     hints.ai_protocol=IPPROTO_TCP;
     sprintf(portstr,"%d",port);
     if ((rc=getaddrinfo(host,portstr,&hints,&list)) != 0) {
          setwsaerr(n,"Could not find server address",rc);
          return(FALSE);
     }

     /* try each address in turn until one accepts the connection */
     lasterr=WSAETIMEDOUT;
     for (ai=list ; ai != NULL ; ai=ai->ai_next) {
          SOCKET s;
          u_long nonblk;
          fd_set wfds,efds;
          struct timeval tv;
          INT soerr,solen;

          s=socket(ai->ai_family,ai->ai_socktype,ai->ai_protocol);
          if (s == INVALID_SOCKET) {
               lasterr=WSAGetLastError();
               continue;
          }

          /*
           * Connect in non-blocking mode so we can give up after the
           * configured timeout instead of the (very long) Windows default.
           */
          nonblk=1;
          ioctlsocket(s,FIONBIO,&nonblk);
          rc=connect(s,ai->ai_addr,(INT)ai->ai_addrlen);
          if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
               lasterr=WSAGetLastError();
               closesocket(s);
               continue;
          }
          FD_ZERO(&wfds);
          FD_ZERO(&efds);
          FD_SET(s,&wfds);
          FD_SET(s,&efds);
          tv.tv_sec=timeoutsec;
          tv.tv_usec=0;
          rc=select(0,NULL,&wfds,&efds,&tv);
          if (rc <= 0) {
               lasterr=(rc == 0) ? WSAETIMEDOUT : WSAGetLastError();
               closesocket(s);
               continue;
          }
          soerr=0;
          solen=sizeof(soerr);
          getsockopt(s,SOL_SOCKET,SO_ERROR,(CHAR *)&soerr,&solen);
          if (FD_ISSET(s,&efds) || soerr != 0) {
               lasterr=(soerr != 0) ? soerr : WSAECONNREFUSED;
               closesocket(s);
               continue;
          }

          /* connected: back to blocking mode with send/receive timeouts */
          nonblk=0;
          ioctlsocket(s,FIONBIO,&nonblk);
          tmo=(DWORD)n->timeout;
          setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const CHAR *)&tmo,sizeof(tmo));
          setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const CHAR *)&tmo,sizeof(tmo));
          getnameinfo(ai->ai_addr,(socklen_t)ai->ai_addrlen,n->peer,
                      sizeof(n->peer),NULL,0,NI_NUMERICHOST);
          n->skt=s;
          break;
     }
     freeaddrinfo(list);
     if (n->skt == INVALID_SOCKET) {
          setwsaerr(n,"Could not connect",lasterr);
          return(FALSE);
     }
     return(TRUE);
}

BOOL                               /*   returns TRUE if TLS is now active  */
sbnetStartTLS(                     /* negotiate TLS on a connected socket  */
SBNET *n,                          /*   connection                         */
BOOL verify)                       /*   verify the server's certificate?   */
{
     SCHANNEL_CRED sc;
     SECURITY_STATUS ss;
     CHAR *newplain;
     INT bufsz;

     /*
      * Any plaintext the server sent that we have not read yet would be
      * an attempt to inject commands into the encrypted session (a known
      * STARTTLS attack).  Refuse rather than carry it across.
      */
     if (!sbnetInputEmpty(n)) {
          seterr(n,"Server sent unexpected data before TLS negotiation");
          return(FALSE);
     }

     /*
      * Credentials: let Windows pick the protocol versions and ciphers
      * (the system defaults are kept current by Windows Update), refuse
      * weak ciphers, and either have Schannel check the certificate
      * chain and host name for us or skip those checks entirely.
      */
     memset(&sc,0,sizeof(sc));
     sc.dwVersion=SCHANNEL_CRED_VERSION;
     sc.dwFlags=SCH_USE_STRONG_CRYPTO|SCH_CRED_NO_DEFAULT_CREDS;
     if (verify) {
          sc.dwFlags|=SCH_CRED_AUTO_CRED_VALIDATION;
     }
     else {
          sc.dwFlags|=SCH_CRED_MANUAL_CRED_VALIDATION
                     |SCH_CRED_NO_SERVERNAME_CHECK;
     }
     ss=AcquireCredentialsHandleA(NULL,UNISP_NAME_A,SECPKG_CRED_OUTBOUND,
                                  NULL,&sc,NULL,NULL,&n->cred,NULL);
     if (ss != SEC_E_OK) {
          setsecerr(n,"TLS setup failed",ss);
          return(FALSE);
     }
     n->havecred=TRUE;

     /* receive buffer big enough for the handshake and any TLS record */
     bufsz=16384+2048;
     if ((n->raw=(CHAR *)malloc(bufsz)) == NULL) {
          seterr(n,"Out of memory");
          return(FALSE);
     }
     n->rawsz=bufsz;
     n->rawlen=0;

     if (!handshake(n,TRUE)) {
          return(FALSE);
     }

     ss=QueryContextAttributesA(&n->ctx,SECPKG_ATTR_STREAM_SIZES,&n->sizes);
     if (ss != SEC_E_OK) {
          setsecerr(n,"TLS setup failed",ss);
          return(FALSE);
     }

     /* make sure a full record (header + data + trailer) fits */
     bufsz=(INT)(n->sizes.cbHeader+n->sizes.cbMaximumMessage
                +n->sizes.cbTrailer);
     if (bufsz > n->rawsz) {
          CHAR *bigger=(CHAR *)realloc(n->raw,bufsz);
          if (bigger == NULL) {
               seterr(n,"Out of memory");
               return(FALSE);
          }
          n->raw=bigger;
          n->rawsz=bufsz;
     }
     if ((n->sndbuf=(CHAR *)malloc(bufsz)) == NULL
      || (newplain=(CHAR *)malloc(bufsz)) == NULL) {
          seterr(n,"Out of memory");
          return(FALSE);
     }
     free(n->plain);
     n->plain=newplain;
     n->plainsz=bufsz;
     n->plainpos=n->plainlen=0;
     n->tls=TRUE;
     describetls(n);
     return(TRUE);
}

BOOL                               /*   returns TRUE if all bytes were sent*/
sbnetSend(                         /* send bytes (encrypted if TLS active) */
SBNET *n,                          /*   connection                         */
const VOID *data,                  /*   bytes to send                      */
INT len)                           /*   number of bytes                    */
{
     const CHAR *p=(const CHAR *)data;

     if (!n->tls) {
          return(sendraw(n,p,len));
     }

     /* TLS: encrypt and send in chunks no larger than one TLS record */
     while (len > 0) {
          SecBuffer buf[4];
          SecBufferDesc desc;
          SECURITY_STATUS ss;
          INT chunk;

          chunk=len;
          if (chunk > (INT)n->sizes.cbMaximumMessage) {
               chunk=(INT)n->sizes.cbMaximumMessage;
          }
          memcpy(n->sndbuf+n->sizes.cbHeader,p,chunk);
          buf[0].BufferType=SECBUFFER_STREAM_HEADER;
          buf[0].pvBuffer=n->sndbuf;
          buf[0].cbBuffer=n->sizes.cbHeader;
          buf[1].BufferType=SECBUFFER_DATA;
          buf[1].pvBuffer=n->sndbuf+n->sizes.cbHeader;
          buf[1].cbBuffer=(ULONG)chunk;
          buf[2].BufferType=SECBUFFER_STREAM_TRAILER;
          buf[2].pvBuffer=n->sndbuf+n->sizes.cbHeader+chunk;
          buf[2].cbBuffer=n->sizes.cbTrailer;
          buf[3].BufferType=SECBUFFER_EMPTY;
          buf[3].pvBuffer=NULL;
          buf[3].cbBuffer=0;
          desc.ulVersion=SECBUFFER_VERSION;
          desc.cBuffers=4;
          desc.pBuffers=buf;
          ss=EncryptMessage(&n->ctx,0,&desc,0);
          if (ss != SEC_E_OK) {
               setsecerr(n,"TLS encryption failed",ss);
               return(FALSE);
          }
          if (!sendraw(n,n->sndbuf,(INT)(buf[0].cbBuffer+buf[1].cbBuffer
                                       +buf[2].cbBuffer))) {
               return(FALSE);
          }
          p+=chunk;
          len-=chunk;
     }
     return(TRUE);
}

INT                                /*   line length, 0 on EOF, -1 on error */
sbnetReadLine(                     /* read one CRLF-terminated line        */
SBNET *n,                          /*   connection                         */
CHAR *line,                        /*   buffer for the line (CRLF removed) */
INT max)                           /*   size of the buffer                 */
{
     INT len=0;
     BOOL any=FALSE;

     /*
      * Collect bytes until LF.  A line longer than the buffer is
      * truncated (the rest is read and discarded) so a misbehaving server
      * can never overrun us.
      */
     for (;;) {
          CHAR c;

          if (n->plainpos >= n->plainlen) {
               INT got=fillplain(n);
               if (got < 0) {
                    return(-1);
               }
               if (got == 0) {
                    if (!any) {
                         seterr(n,"Server closed the connection");
                         return(0);
                    }
                    break;
               }
          }
          c=n->plain[n->plainpos++];
          any=TRUE;
          if (c == '\n') {
               break;
          }
          if (len < max-1) {
               line[len++]=c;
          }
     }
     if (len > 0 && line[len-1] == '\r') {
          len--;
     }
     line[len]='\0';
     /* report at least 1 so an empty line is not mistaken for EOF */
     return(len > 0 ? len : 1);
}

INT                                /*   bytes read, 0 on EOF, -1 on error  */
sbnetRead(                         /* read whatever bytes are available    */
SBNET *n,                          /*   connection                         */
VOID *buf,                         /*   buffer for the bytes               */
INT max)                           /*   size of the buffer                 */
{
     INT len;

     /*
      * Unlike sbnetReadLine() nothing is cut off or changed, so a caller
      * that must keep every byte (a downloaded mail message) can do its
      * own line handling.  Waits only when nothing is buffered.
      */
     if (n->plainpos >= n->plainlen) {
          INT got=fillplain(n);
          if (got <= 0) {
               if (got == 0) {
                    seterr(n,"Server closed the connection");
               }
               return(got);
          }
     }
     len=n->plainlen-n->plainpos;
     if (len > max) {
          len=max;
     }
     memcpy(buf,&n->plain[n->plainpos],len);
     n->plainpos+=len;
     return(len);
}

BOOL                               /*   TRUE if no unread input is buffered*/
sbnetInputEmpty(                   /* check for unread received data       */
SBNET *n)                          /*   connection                         */
{
     return(n->plainpos >= n->plainlen && n->rawlen == 0);
}

VOID
sbnetAbort(                        /* force a blocked connection to fail   */
SBNET *n)                          /*   (safe to call from another thread) */
{
     SOCKET s=n->skt;

     /*
      * shutdown() makes a recv()/send() blocked in another thread return
      * at once with an error.  The owning thread still closes the socket
      * itself, so the handle is never closed twice.
      */
     if (s != INVALID_SOCKET) {
          shutdown(s,SD_BOTH);
     }
}

VOID
sbnetClose(                        /* shut down TLS, close, free buffers   */
SBNET *n)                          /*   connection                         */
{
     if (n->tls && n->havectx && n->skt != INVALID_SOCKET) {
          /*
           * Polite TLS close: ask Schannel for a "close_notify" alert and
           * send it, so the server knows the session ended on purpose.
           */
          DWORD type=SCHANNEL_SHUTDOWN;
          SecBuffer buf;
          SecBufferDesc desc;
          SecBuffer outbuf;
          SecBufferDesc outdesc;
          DWORD flags,outflags;

          buf.BufferType=SECBUFFER_TOKEN;
          buf.pvBuffer=&type;
          buf.cbBuffer=sizeof(type);
          desc.ulVersion=SECBUFFER_VERSION;
          desc.cBuffers=1;
          desc.pBuffers=&buf;
          if (ApplyControlToken(&n->ctx,&desc) == SEC_E_OK) {
               flags=ISC_REQ_SEQUENCE_DETECT|ISC_REQ_REPLAY_DETECT
                    |ISC_REQ_CONFIDENTIALITY|ISC_REQ_EXTENDED_ERROR
                    |ISC_REQ_ALLOCATE_MEMORY|ISC_REQ_STREAM;
               outbuf.BufferType=SECBUFFER_TOKEN;
               outbuf.pvBuffer=NULL;
               outbuf.cbBuffer=0;
               outdesc.ulVersion=SECBUFFER_VERSION;
               outdesc.cBuffers=1;
               outdesc.pBuffers=&outbuf;
               if (InitializeSecurityContextA(&n->cred,&n->ctx,NULL,flags,0,0,
                                              NULL,0,NULL,&outdesc,&outflags,
                                              NULL) == SEC_E_OK
                && outbuf.pvBuffer != NULL) {
                    send(n->skt,(const CHAR *)outbuf.pvBuffer,
                         (INT)outbuf.cbBuffer,0);
               }
               if (outbuf.pvBuffer != NULL) {
                    FreeContextBuffer(outbuf.pvBuffer);
               }
          }
     }
     if (n->havectx) {
          DeleteSecurityContext(&n->ctx);
          n->havectx=FALSE;
     }
     if (n->havecred) {
          FreeCredentialsHandle(&n->cred);
          n->havecred=FALSE;
     }
     if (n->skt != INVALID_SOCKET) {
          shutdown(n->skt,SD_SEND);
          closesocket(n->skt);
          n->skt=INVALID_SOCKET;
     }
     free(n->raw);
     free(n->plain);
     free(n->sndbuf);
     n->raw=n->plain=n->sndbuf=NULL;
     n->rawlen=n->plainlen=n->plainpos=0;
     n->tls=FALSE;
}

/*
 * Run the TLS handshake.  With initial=TRUE a new session is started;
 * with initial=FALSE the server has sent more handshake messages during
 * an established session (renegotiation, or TLS 1.3 post-handshake
 * messages) and the bytes waiting in n->raw are fed back to Schannel.
 */
static BOOL
handshake(
SBNET *n,
BOOL initial)
{
     DWORD flags,outflags;
     SECURITY_STATUS ss;
     SecBuffer inbuf[2],outbuf[1];
     SecBufferDesc indesc,outdesc;
     BOOL needread;

     flags=ISC_REQ_SEQUENCE_DETECT|ISC_REQ_REPLAY_DETECT
          |ISC_REQ_CONFIDENTIALITY|ISC_REQ_EXTENDED_ERROR
          |ISC_REQ_ALLOCATE_MEMORY|ISC_REQ_STREAM;

     if (initial) {
          /* first call produces the "ClientHello" message for the server */
          outbuf[0].BufferType=SECBUFFER_TOKEN;
          outbuf[0].pvBuffer=NULL;
          outbuf[0].cbBuffer=0;
          outdesc.ulVersion=SECBUFFER_VERSION;
          outdesc.cBuffers=1;
          outdesc.pBuffers=outbuf;
          ss=InitializeSecurityContextA(&n->cred,NULL,(SEC_CHAR *)n->host,
                                        flags,0,0,NULL,0,&n->ctx,&outdesc,
                                        &outflags,NULL);
          if (ss != SEC_I_CONTINUE_NEEDED) {
               setsecerr(n,"TLS negotiation failed",ss);
               return(FALSE);
          }
          n->havectx=TRUE;
          if (outbuf[0].cbBuffer != 0 && outbuf[0].pvBuffer != NULL) {
               BOOL ok=sendraw(n,(const CHAR *)outbuf[0].pvBuffer,
                               (INT)outbuf[0].cbBuffer);
               FreeContextBuffer(outbuf[0].pvBuffer);
               if (!ok) {
                    return(FALSE);
               }
          }
          needread=TRUE;
     }
     else {
          needread=(n->rawlen == 0);
     }

     for (;;) {
          if (needread) {
               INT got;

               if (n->rawlen >= n->rawsz) {
                    seterr(n,"TLS negotiation failed (handshake too large)");
                    return(FALSE);
               }
               got=recvraw(n,n->raw+n->rawlen,n->rawsz-n->rawlen);
               if (got <= 0) {
                    if (got == 0) {
                         seterr(n,"Server closed the connection during TLS "
                                  "negotiation (is this a TLS port?)");
                    }
                    return(FALSE);
               }
               n->rawlen+=got;
          }

          inbuf[0].BufferType=SECBUFFER_TOKEN;
          inbuf[0].pvBuffer=n->raw;
          inbuf[0].cbBuffer=(ULONG)n->rawlen;
          inbuf[1].BufferType=SECBUFFER_EMPTY;
          inbuf[1].pvBuffer=NULL;
          inbuf[1].cbBuffer=0;
          indesc.ulVersion=SECBUFFER_VERSION;
          indesc.cBuffers=2;
          indesc.pBuffers=inbuf;
          outbuf[0].BufferType=SECBUFFER_TOKEN;
          outbuf[0].pvBuffer=NULL;
          outbuf[0].cbBuffer=0;
          outdesc.ulVersion=SECBUFFER_VERSION;
          outdesc.cBuffers=1;
          outdesc.pBuffers=outbuf;

          ss=InitializeSecurityContextA(&n->cred,&n->ctx,(SEC_CHAR *)n->host,
                                        flags,0,0,&indesc,0,NULL,&outdesc,
                                        &outflags,NULL);

          if (ss == SEC_E_INCOMPLETE_MESSAGE) {
               needread=TRUE;      /* partial record: wait for the rest   */
               continue;
          }

          /* anything Schannel wants sent goes to the server right away */
          if (outbuf[0].cbBuffer != 0 && outbuf[0].pvBuffer != NULL) {
               BOOL ok=sendraw(n,(const CHAR *)outbuf[0].pvBuffer,
                               (INT)outbuf[0].cbBuffer);
               FreeContextBuffer(outbuf[0].pvBuffer);
               if (!ok && !FAILED(ss)) {
                    return(FALSE);
               }
          }

          if (FAILED(ss)) {
               setsecerr(n,"TLS negotiation failed",ss);
               return(FALSE);
          }

          /* keep any bytes that belong to the next message */
          if (inbuf[1].BufferType == SECBUFFER_EXTRA && inbuf[1].cbBuffer > 0) {
               memmove(n->raw,n->raw+(n->rawlen-inbuf[1].cbBuffer),
                       inbuf[1].cbBuffer);
               n->rawlen=(INT)inbuf[1].cbBuffer;
          }
          else {
               n->rawlen=0;
          }

          if (ss == SEC_E_OK) {
               return(TRUE);       /* handshake complete                  */
          }
          if (ss == SEC_I_INCOMPLETE_CREDENTIALS) {
               /* server asked for a client certificate; continue without */
               needread=FALSE;
               continue;
          }
          if (ss == SEC_I_CONTINUE_NEEDED) {
               needread=(n->rawlen == 0);
               continue;
          }
          setsecerr(n,"TLS negotiation failed",ss);
          return(FALSE);
     }
}

/*
 * Make plaintext available in n->plain.  Returns the number of bytes now
 * available, 0 when the server has closed the connection, -1 on error.
 */
static INT
fillplain(
SBNET *n)
{
     n->plainpos=n->plainlen=0;
     if (!n->tls) {
          INT got=recvraw(n,n->plain,n->plainsz);
          if (got > 0) {
               n->plainlen=got;
          }
          return(got);
     }
     for (;;) {
          if (n->rawlen > 0) {
               SecBuffer buf[4];
               SecBufferDesc desc;
               SECURITY_STATUS ss;
               INT i;
               SecBuffer *data=NULL,*extra=NULL;

               buf[0].BufferType=SECBUFFER_DATA;
               buf[0].pvBuffer=n->raw;
               buf[0].cbBuffer=(ULONG)n->rawlen;
               for (i=1 ; i < 4 ; i++) {
                    buf[i].BufferType=SECBUFFER_EMPTY;
                    buf[i].pvBuffer=NULL;
                    buf[i].cbBuffer=0;
               }
               desc.ulVersion=SECBUFFER_VERSION;
               desc.cBuffers=4;
               desc.pBuffers=buf;
               ss=DecryptMessage(&n->ctx,&desc,0,NULL);
               if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                    /* need more bytes to complete this record: read below */
               }
               else if (ss == SEC_I_CONTEXT_EXPIRED) {
                    return(0);     /* server sent close_notify           */
               }
               else if (ss == SEC_E_OK || ss == SEC_I_RENEGOTIATE) {
                    for (i=1 ; i < 4 ; i++) {
                         if (buf[i].BufferType == SECBUFFER_DATA) {
                              data=&buf[i];
                         }
                         else if (buf[i].BufferType == SECBUFFER_EXTRA) {
                              extra=&buf[i];
                         }
                    }
                    if (data != NULL && data->cbBuffer > 0) {
                         memcpy(n->plain,data->pvBuffer,data->cbBuffer);
                         n->plainlen=(INT)data->cbBuffer;
                    }
                    if (extra != NULL && extra->cbBuffer > 0) {
                         memmove(n->raw,n->raw+(n->rawlen-extra->cbBuffer),
                                 extra->cbBuffer);
                         n->rawlen=(INT)extra->cbBuffer;
                    }
                    else {
                         n->rawlen=0;
                    }
                    if (ss == SEC_I_RENEGOTIATE) {
                         /* server sent handshake messages; process them */
                         if (!handshake(n,FALSE)) {
                              return(-1);
                         }
                    }
                    if (n->plainlen > 0) {
                         return(n->plainlen);
                    }
                    continue;      /* empty record: try the next one      */
               }
               else {
                    setsecerr(n,"TLS decryption failed",ss);
                    return(-1);
               }
          }
          {
               INT got;

               if (n->rawlen >= n->rawsz) {
                    seterr(n,"TLS record too large");
                    return(-1);
               }
               got=recvraw(n,n->raw+n->rawlen,n->rawsz-n->rawlen);
               if (got <= 0) {
                    return(got);
               }
               n->rawlen+=got;
          }
     }
}

static BOOL
sendraw(                           /* send bytes on the socket as-is       */
SBNET *n,
const CHAR *data,
INT len)
{
     while (len > 0) {
          INT sent=send(n->skt,data,len,0);
          if (sent == SOCKET_ERROR) {
               setwsaerr(n,"Send failed",WSAGetLastError());
               return(FALSE);
          }
          data+=sent;
          len-=sent;
     }
     return(TRUE);
}

static INT                         /*   bytes read, 0 on EOF, -1 on error  */
recvraw(                           /* receive bytes from the socket as-is  */
SBNET *n,
CHAR *buf,
INT len)
{
     INT got;

     if (len <= 0) {
          return(0);
     }
     got=recv(n->skt,buf,len,0);
     if (got == SOCKET_ERROR) {
          INT code=WSAGetLastError();
          if (code == WSAETIMEDOUT) {
               seterr(n,"Timed out waiting for the server");
          }
          else {
               setwsaerr(n,"Receive failed",code);
          }
          return(-1);
     }
     return(got);
}

static VOID
describetls(                       /* record the negotiated TLS version    */
SBNET *n)
{
     SecPkgContext_ConnectionInfo ci;
     const CHAR *ver="TLS";

     if (QueryContextAttributesA(&n->ctx,SECPKG_ATTR_CONNECTION_INFO,&ci)
         == SEC_E_OK) {
          if (ci.dwProtocol & SP_PROT_TLS1_3_CLIENT) {
               ver="TLS 1.3";
          }
          else if (ci.dwProtocol & SP_PROT_TLS1_2_CLIENT) {
               ver="TLS 1.2";
          }
          else if (ci.dwProtocol & SP_PROT_TLS1_1_CLIENT) {
               ver="TLS 1.1";
          }
          else if (ci.dwProtocol & SP_PROT_TLS1_0_CLIENT) {
               ver="TLS 1.0";
          }
     }
     strncpy(n->tlsinfo,ver,sizeof(n->tlsinfo)-1);
     n->tlsinfo[sizeof(n->tlsinfo)-1]='\0';
}

static VOID
seterr(                            /* record an error message              */
SBNET *n,
const CHAR *fmt,
...)
{
     va_list ap;

     va_start(ap,fmt);
     _vsnprintf(n->err,sizeof(n->err)-1,fmt,ap);
     va_end(ap);
     n->err[sizeof(n->err)-1]='\0';
}

static VOID
setwsaerr(                         /* record a Winsock error message       */
SBNET *n,
const CHAR *what,
INT code)
{
     CHAR text[160];
     DWORD len;

     len=FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL,(DWORD)code,0,text,sizeof(text),NULL);
     while (len > 0 && (text[len-1] == '\r' || text[len-1] == '\n'
                     || text[len-1] == '.' || text[len-1] == ' ')) {
          text[--len]='\0';
     }
     if (len == 0) {
          strcpy(text,"unknown error");
     }
     seterr(n,"%s: %s (%d)",what,text,code);
}

static VOID
setsecerr(                         /* record a TLS (Schannel) error        */
SBNET *n,
const CHAR *what,
SECURITY_STATUS ss)
{
     const CHAR *why;

     /* plain-English explanations for the problems sysops will hit */
     switch (ss) {
     case SEC_E_UNTRUSTED_ROOT:
          why="server certificate is not trusted";
          break;
     case SEC_E_WRONG_PRINCIPAL:
          why="server certificate does not match the host name";
          break;
     case SEC_E_CERT_EXPIRED:
          why="server certificate has expired";
          break;
     case CERT_E_REVOKED:
     case CRYPT_E_REVOKED:
          why="server certificate has been revoked";
          break;
     case SEC_E_ILLEGAL_MESSAGE:
          why="server did not speak TLS (wrong port or security mode?)";
          break;
     case SEC_E_ALGORITHM_MISMATCH:
          why="no TLS version or cipher in common with the server";
          break;
     case SEC_E_INVALID_TOKEN:
          why="server did not answer in TLS (wrong port or security mode?)";
          break;
     case SEC_E_INTERNAL_ERROR:
          why="Windows TLS internal error";
          break;
     default:
          why="security error";
          break;
     }
     seterr(n,"%s: %s (0x%08lX)",what,why,(unsigned long)ss);
}
