/***************************************************************************
 *                                                                         *
 *   BNWORK.H                                                              *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   The news worker thread.  One job is one news server session:         *
 *                                                                         *
 *     1. connect and log in                                               *
 *     2. post the forum messages waiting to go out to Usenet              *
 *     3. for each newsgroup, download the articles not seen yet, decode   *
 *        them, and leave them in the spool folder for the main thread     *
 *     4. QUIT                                                             *
 *                                                                         *
 *   The BBS runs every user on one thread, so anything slow (DNS, TCP,   *
 *   TLS, waiting for a news server) must happen here or the whole board   *
 *   would freeze.  The main thread copies everything the session needs   *
 *   into a BNJOB and collects the outcome on its next timer tick.  The    *
 *   worker makes no BBS SDK calls and reads no BBS globals.               *
 *                                                                         *
 ***************************************************************************/

#ifndef BNWORK_H
#define BNWORK_H

#include "BNNNTP.H"
#include "BNSPOOL.H"

#ifdef __cplusplus
extern "C" {
#endif

#define BNW_MAXGRP      64         /* newsgroups per session               */
#define BNW_MAXPOST     32         /* articles posted per session          */
#define BNW_MAXNEW      1000       /* articles downloaded per session      */
#define BNW_STRSZ       256        /* room for each copied setting         */
#define BNW_ERRSZ       128        /* room for a per-item error            */
#define BNW_BACKALL     0x7FFFFFFF /* backfill: every article still there  */

                                   /* outcome of one post                  */
#define BNP_NOTTRIED    0          /*   session ended before it was tried  */
#define BNP_OK          1          /*   the server accepted it             */
#define BNP_TEMP        2          /*   failed for now: try again later    */
#define BNP_PERM        3          /*   refused for good                   */

typedef struct bnwgrp {            /* one newsgroup to read                */
     CHAR name[BNSP_GROUPSZ];      /*   in:  newsgroup name                */
     BNARTNUM next;                /*   in:  next article to read (0 = new */
                                   /*        group: start with backfill)   */
     BOOL tried;                   /*   out: GROUP was attempted           */
     BOOL ok;                      /*   out: group opened                  */
     CHAR err[BNW_ERRSZ];          /*   out: why not                       */
     BNARTNUM first,last,count;    /*   out: what the server reported      */
     BNARTNUM newnext;             /*   out: next article to read from now */
     INT nfetched;                 /*   out: articles put in the spool     */
     INT nmissing;                 /*   out: numbers with no article       */
     INT nbad;                     /*   out: articles that failed          */
     BOOL renumbered;              /*   out: server's numbers went down    */
} BNWGRP;

typedef struct bnwpost {           /* one article to post                  */
     LONG qid;                     /*   in:  outgoing queue id             */
     CHAR artfile[MAX_PATH];       /*   in:  the article file              */
     INT status;                   /*   out: BNP_xxx                       */
     INT code;                     /*   out: server's reply code           */
     CHAR text[BNW_ERRSZ];         /*   out: server's reply or the error   */
} BNWPOST;

typedef struct bnjob {             /* one session, self-contained          */
     /* --- settings, filled in by the main thread --- */
     CHAR host[BNW_STRSZ];         /*   news server                        */
     INT port;
     INT security;                 /*   BNSEC_xxx                          */
     BOOL verify;                  /*   check the server's certificate?    */
     CHAR user[BNW_STRSZ];         /*   login ("" = none)                  */
     CHAR pass[BNW_STRSZ];
     INT timeout;                  /*   seconds per step                   */
     CHAR spooldir[MAX_PATH];      /*   incoming spool, ending in '\'      */
     CHAR logfile[MAX_PATH];       /*   transcript log ("" = no log)       */
     LONG maxbytes;                /*   larger articles: header only       */
     INT maxper;                   /*   most articles per group            */
     INT backfill;                 /*   articles to read for a new group   */
     INT wrapcol;                  /*   word-wrap column (0 = none)        */
     INT maxnew;                   /*   most articles this session         */
     BOOL dofetch;                 /*   read the groups (else post only)   */
     INT ngrp;
     BNWGRP grp[BNW_MAXGRP];
     INT npost;
     BNWPOST post[BNW_MAXPOST];

     /* --- outcome, filled in by the worker --- */
     BOOL ok;                      /*   connected and logged in            */
     CHAR stage[16];               /*   step that failed                   */
     CHAR err[SBNET_ERRSZ];        /*   why                                */
     CHAR tlsinfo[SBNET_INFOSZ];   /*   "TLS 1.3" etc., "" if not encrypted*/
     INT nnew;                     /*   spool ids written                  */
     CHAR newsid[BNW_MAXNEW][BNSP_IDSZ];
} BNJOB;

BOOL                               /*   TRUE if the worker is running      */
bnwStart(VOID);                    /* start the worker thread              */

VOID
bnwStop(                           /* stop the worker thread               */
DWORD waitms);                     /*   how long to wait for it to finish  */

BOOL                               /*   TRUE if a session is in progress or*/
bnwBusy(VOID);                     /*   its result has not been collected  */

BOOL                               /*   FALSE if the worker is busy        */
bnwSubmit(                         /* hand a job to the worker             */
const BNJOB *job);                 /*   job (copied; caller may reuse it)  */

BOOL                               /*   TRUE if a finished job was returned*/
bnwCollect(                        /* pick up a finished job               */
BNJOB *job);                       /*   receives the job and its outcome   */

VOID
bnwRun(                            /* run one session now, on the calling  */
BNJOB *job,                        /*   thread (the worker, or the test    */
BNNNTP *s,                         /*   tool); s is bnnInit'ed by caller   */
volatile LONG *stop);              /*   set non-zero to stop early (NULL)  */

BOOL                               /*   TRUE if the article was spooled    */
bnwSpoolArticle(                   /* decode one article into the spool    */
const CHAR *spooldir,              /*   (shared with the test tool)        */
const CHAR *group,
BNARTNUM artnum,
const CHAR *raw,                   /*   article as received (CRLF)         */
size_t len,
BOOL truncated,                    /*   only part of it was kept           */
INT wrapcol,
const CHAR *host,                  /*   server name, for a made-up id      */
CHAR *sid);                        /*   receives the spool id              */

BNARTNUM                           /*   first article number to read       */
bnwStartAt(                        /* where a group's session starts       */
BNARTNUM next,                     /*   saved next article (0 = new group) */
BNARTNUM first,                    /*   server's lowest article number     */
BNARTNUM last,                     /*   server's highest article number    */
INT backfill,                      /*   articles for a new group, or       */
                                   /*   BNW_BACKALL for all of them        */
BOOL *renumbered);                 /*   set TRUE if the numbers went down  */

VOID
bnwWipe(                           /* erase the password from a job        */
BNJOB *job);

#ifdef __cplusplus
}
#endif

#endif /* BNWORK_H */
