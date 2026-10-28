/***************************************************************************
 *                                                                         *
 *   BNWORK.C                                                              *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   The news worker thread.  One job at a time:                          *
 *                                                                         *
 *     main thread                      worker thread                     *
 *     -----------                      -------------                     *
 *     bnwSubmit(job)     --(event)-->  runs the session (bnwRun)          *
 *     ...board keeps running...        writes the transcript log          *
 *     bnwCollect(&job)   <----------   marks the job done                 *
 *                                                                         *
 *   The job slot is protected by a critical section; the worker only     *
 *   touches its own copy of the job during the session, so the main      *
 *   thread never waits on the network.                                    *
 *                                                                         *
 *   No BBS SDK calls are made from this file.                             *
 *                                                                         *
 ***************************************************************************/

#include "BNWORK.H"
#include "IEMSG.H"
#include "SBMAIL.H"
#include "SBUTIL.H"
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define SLOT_EMPTY  0              /* no job                               */
#define SLOT_QUEUED 1              /* submitted, worker has not started    */
#define SLOT_ACTIVE 2              /* worker is running it                 */
#define SLOT_DONE   3              /* finished, waiting to be collected    */

static HANDLE thread=NULL;         /* the worker thread                    */
static HANDLE wake=NULL;           /* signalled when a job is submitted    */
static CRITICAL_SECTION lock;      /* protects everything below            */
static BOOL started=FALSE;         /* worker running?                      */
static volatile LONG stopping=0;   /* shutdown requested?                  */
static INT slotstate=SLOT_EMPTY;   /* SLOT_xxx                             */
static BNJOB *slot=NULL;           /* the one job slot                     */
static BNJOB *mine=NULL;           /* the worker's private copy            */
static BNNNTP *active=NULL;        /* session in progress (for aborting)   */

typedef struct logctx {            /* transcript log being written         */
     FILE *fp;
} LOGCTX;

static unsigned __stdcall workmain(VOID *arg);
static VOID logline(VOID *ctx, const CHAR *line);
static VOID postall(BNJOB *job, BNNNTP *s);
static BOOL fetchgroup(BNJOB *job, BNWGRP *g, BNNNTP *s, volatile LONG *stop);
static VOID failposts(BNJOB *job, const CHAR *why);
static BOOL lost(BNNNTP *s);

BOOL                               /*   TRUE if the worker is running      */
bnwStart(VOID)                     /* start the worker thread              */
{
     unsigned tid;

     if (started) {
          return(TRUE);
     }
     if ((slot=(BNJOB *)calloc(1,sizeof(BNJOB))) == NULL
      || (mine=(BNJOB *)calloc(1,sizeof(BNJOB))) == NULL) {
          free(slot);
          slot=NULL;
          return(FALSE);
     }
     InitializeCriticalSection(&lock);
     stopping=0;
     slotstate=SLOT_EMPTY;
     if ((wake=CreateEventA(NULL,FALSE,FALSE,NULL)) == NULL) {
          DeleteCriticalSection(&lock);
          return(FALSE);
     }
     thread=(HANDLE)_beginthreadex(NULL,0,workmain,NULL,0,&tid);
     if (thread == NULL) {
          CloseHandle(wake);
          wake=NULL;
          DeleteCriticalSection(&lock);
          return(FALSE);
     }
     started=TRUE;
     return(TRUE);
}

VOID
bnwStop(                           /* stop the worker thread               */
DWORD waitms)
{
     if (!started) {
          return;
     }
     /*
      * Ask the worker to stop, and if it is waiting on a slow server,
      * make its socket fail at once so it notices.
      */
     InterlockedExchange(&stopping,1);
     EnterCriticalSection(&lock);
     if (active != NULL) {
          sbnetAbort(&active->net);
     }
     LeaveCriticalSection(&lock);
     SetEvent(wake);
     if (WaitForSingleObject(thread,waitms) == WAIT_OBJECT_0) {
          CloseHandle(thread);
          CloseHandle(wake);
          DeleteCriticalSection(&lock);
          SecureZeroMemory(slot,sizeof(*slot));
          SecureZeroMemory(mine,sizeof(*mine));
          free(slot);
          free(mine);
          slot=mine=NULL;
     }
     /* (if it did not finish, leave everything: the thread still uses it) */
     thread=NULL;
     started=FALSE;
}

BOOL                               /*   TRUE if a session is in progress or*/
bnwBusy(VOID)                      /*   its result has not been collected  */
{
     BOOL busy;

     if (!started) {
          return(TRUE);
     }
     EnterCriticalSection(&lock);
     busy=(slotstate != SLOT_EMPTY);
     LeaveCriticalSection(&lock);
     return(busy);
}

BOOL                               /*   FALSE if the worker is busy        */
bnwSubmit(                         /* hand a job to the worker             */
const BNJOB *job)
{
     BOOL ok=FALSE;

     if (!started) {
          return(FALSE);
     }
     EnterCriticalSection(&lock);
     if (slotstate == SLOT_EMPTY) {
          memcpy(slot,job,sizeof(*slot));
          slotstate=SLOT_QUEUED;
          ok=TRUE;
     }
     LeaveCriticalSection(&lock);
     if (ok) {
          SetEvent(wake);
     }
     return(ok);
}

BOOL                               /*   TRUE if a finished job was returned*/
bnwCollect(                        /* pick up a finished job               */
BNJOB *job)
{
     BOOL ok=FALSE;

     if (!started) {
          return(FALSE);
     }
     EnterCriticalSection(&lock);
     if (slotstate == SLOT_DONE) {
          memcpy(job,slot,sizeof(*job));
          SecureZeroMemory(slot,sizeof(*slot));
          slotstate=SLOT_EMPTY;
          ok=TRUE;
     }
     LeaveCriticalSection(&lock);
     return(ok);
}

VOID
bnwWipe(                           /* erase the password from a job        */
BNJOB *job)
{
     SecureZeroMemory(job->pass,sizeof(job->pass));
}

static unsigned __stdcall
workmain(                          /* the worker thread itself             */
VOID *arg)
{
     BNNNTP *s;

     (VOID)arg;
     if ((s=(BNNNTP *)malloc(sizeof(BNNNTP))) == NULL) {
          return(1);
     }
     for (;;) {
          WaitForSingleObject(wake,INFINITE);
          if (stopping) {
               break;
          }
          /* take a private copy of the job and mark the slot active */
          EnterCriticalSection(&lock);
          if (slotstate != SLOT_QUEUED) {
               LeaveCriticalSection(&lock);
               continue;
          }
          memcpy(mine,slot,sizeof(*mine));
          slotstate=SLOT_ACTIVE;
          bnnInit(s,NULL,NULL);
          active=s;                /* published so a shutdown can abort it */
          LeaveCriticalSection(&lock);

          bnwRun(mine,s,&stopping);

          /* hand the outcome back */
          EnterCriticalSection(&lock);
          active=NULL;
          memcpy(slot,mine,sizeof(*slot));
          slotstate=SLOT_DONE;
          LeaveCriticalSection(&lock);
          SecureZeroMemory(mine,sizeof(*mine));
          if (stopping) {
               break;
          }
     }
     SecureZeroMemory(s,sizeof(*s));
     free(s);
     return(0);
}

/* ======================================================================== */
/*   One session                                                             */
/* ======================================================================== */

VOID
bnwRun(                            /* run one session now                  */
BNJOB *job,
BNNNTP *s,
volatile LONG *stop)
{
     LOGCTX lc;
     CHAR stamp[64];
     time_t now;
     INT i;

     job->ok=FALSE;
     job->stage[0]=job->err[0]=job->tlsinfo[0]='\0';
     job->nnew=0;

     /* optional transcript log, appended to (one block per session) */
     memset(&lc,0,sizeof(lc));
     if (job->logfile[0] != '\0' && (lc.fp=fopen(job->logfile,"a")) != NULL) {
          time(&now);
          strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M:%S",localtime(&now));
          fprintf(lc.fp,"%s session: %d group(s), %d post(s)\n",stamp,
                  job->dofetch ? job->ngrp : 0,job->npost);
          s->log=logline;
          s->logctx=&lc;
     }

     sbStrCopy(job->stage,"CONNECT",sizeof(job->stage));
     if (!bnnConnect(s,job->host,job->port,job->security,job->verify,
                     job->timeout)) {
          sbStrCopy(job->err,s->err,sizeof(job->err));
          failposts(job,job->err);
          goto done;
     }
     if (s->net.tls) {
          sbStrCopy(job->tlsinfo,s->net.tlsinfo,sizeof(job->tlsinfo));
     }
     sbStrCopy(job->stage,"LOGIN",sizeof(job->stage));
     if (!bnnAuth(s,job->user,job->pass)) {
          sbStrCopy(job->err,s->err,sizeof(job->err));
          failposts(job,job->err);
          goto done;
     }
     job->ok=TRUE;
     job->stage[0]='\0';

     /* posts first, so a reply goes out without waiting for downloads */
     sbStrCopy(job->stage,"POST",sizeof(job->stage));
     postall(job,s);
     if (lost(s)) {
          sbStrCopy(job->err,s->err,sizeof(job->err));
          goto done;
     }

     if (job->dofetch) {
          sbStrCopy(job->stage,"READ",sizeof(job->stage));
          for (i=0 ; i < job->ngrp ; i++) {
               if ((stop != NULL && *stop) || job->nnew >= job->maxnew) {
                    break;
               }
               if (!fetchgroup(job,&job->grp[i],s,stop)) {
                    sbStrCopy(job->err,s->err,sizeof(job->err));
                    goto done;     /* connection lost                     */
               }
          }
     }
     job->stage[0]='\0';
     bnnQuit(s);

done:
     bnnClose(s);
     if (lc.fp != NULL) {
          fprintf(lc.fp,"  => %s%s%s; %d new article(s)\n\n",
                  job->err[0] == '\0' ? "OK" : job->stage,
                  job->err[0] == '\0' ? "" : ": ",job->err,job->nnew);
          fclose(lc.fp);
     }
     s->log=NULL;
     s->logctx=NULL;
}

/*
 * Post every queued article.  The outcome of each is decided from the
 * server's reply:
 *   240                         accepted
 *   441 naming a duplicate      accepted earlier (a retry after the
 *                               connection dropped before the reply)
 *   441 otherwise               refused for good (bad article)
 *   anything else, or no reply  temporary (login, permission, network):
 *                               tried again later
 */
static VOID
postall(
BNJOB *job,
BNNNTP *s)
{
     INT i;

     for (i=0 ; i < job->npost ; i++) {
          BNWPOST *p=&job->post[i];
          CHAR *art;
          LONG len;
          INT code;

          if ((art=sbReadFile(p->artfile,&len,4L*1024L*1024L)) == NULL) {
               p->status=BNP_PERM;
               sbStrCopy(p->text,"The article file is missing or unreadable",
                         sizeof(p->text));
               continue;
          }
          code=bnnPost(s,art,(size_t)len);
          free(art);
          p->code=code;
          if (code < 0) {
               p->status=BNP_TEMP;
               sbStrCopy(p->text,s->err,sizeof(p->text));
               failposts(job,s->err);       /* the rest were not tried    */
               return;
          }
          sbStrCopy(p->text,s->reply,sizeof(p->text));
          if (code == 240) {
               p->status=BNP_OK;
          }
          else if (code == 441) {
               CHAR low[BNN_LINESZ];

               sbStrCopy(low,s->reply,sizeof(low));
               _strlwr(low);
               p->status=(strstr(low,"435") != NULL
                       || strstr(low,"duplicate") != NULL
                       || strstr(low,"already") != NULL) ? BNP_OK : BNP_PERM;
          }
          else {
               p->status=BNP_TEMP;
          }
     }
}

/*
 * Where to start reading a group:
 *   new group (next = 0)  the last "backfill" articles, or every article
 *                         the server still has for BNW_BACKALL
 *   next > last+1         the server's numbering went backwards (it was
 *                         rebuilt): start over like a new group; articles
 *                         already imported are skipped by Message-ID on
 *                         the BBS side
 *   otherwise             where we left off
 * Never before the server's first article.  The session limits then read
 * a large backlog a few hundred articles at a time, check after check.
 */
BNARTNUM
bnwStartAt(
BNARTNUM next,
BNARTNUM first,
BNARTNUM last,
INT backfill,
BOOL *renumbered)
{
     BNARTNUM start;

     if (next <= 0 || next > last+1) {
          if (next > last+1) {
               *renumbered=TRUE;
          }
          if (backfill >= BNW_BACKALL) {
               start=first;
          }
          else {
               start=last-(BNARTNUM)backfill+1;
          }
     }
     else {
          start=next;
     }
     if (start < first) {
          start=first;
     }
     return(start);
}

/*
 * Read one newsgroup.  Returns FALSE only when the connection was lost
 * (the session cannot go on); a group the server refuses is noted in the
 * group's outcome and the session moves on to the next one.
 */
static BOOL
fetchgroup(
BNJOB *job,
BNWGRP *g,
BNNNTP *s,
volatile LONG *stop)
{
     BNARTNUM n,start;
     INT taken=0;

     g->tried=TRUE;
     g->newnext=g->next;
     if (!bnnGroup(s,g->name,&g->count,&g->first,&g->last)) {
          sbStrCopy(g->err,s->err,sizeof(g->err));
          return(!lost(s));
     }
     g->ok=TRUE;
     start=bnwStartAt(g->next,g->first,g->last,job->backfill,&g->renumbered);
     if (g->last < g->first || g->count == 0) {
          g->newnext=(g->last+1 > start) ? g->last+1 : start;
          return(TRUE);            /* nothing to read                     */
     }

     for (n=start ; n <= g->last ; n++) {
          CHAR *buf,sid[BNSP_IDSZ];
          size_t len;
          BOOL trunc;
          INT code;

          /*
           * Limits per session: articles taken, and numbers tried (a run
           * of expired numbers costs a round trip each); the rest is read
           * next time.
           */
          if (taken >= job->maxper || job->nnew >= job->maxnew
           || n-start >= (BNARTNUM)job->maxper*4
           || (stop != NULL && *stop)) {
               break;
          }
          code=bnnArticle(s,n,&buf,&len,(size_t)job->maxbytes,&trunc);
          if (code < 0) {
               g->newnext=n;       /* not read: start here next time      */
               sbStrCopy(g->err,s->err,sizeof(g->err));
               return(FALSE);
          }
          if (code == 423 || code == 430) {
               g->nmissing++;      /* cancelled or expired: nothing there */
               continue;
          }
          if (code != 220) {
               sbStrCopy(g->err,s->reply,sizeof(g->err));
               g->nbad++;
               break;              /* refused: try this one again later   */
          }
          taken++;
          if (bnwSpoolArticle(job->spooldir,g->name,n,buf,len,trunc,
                              job->wrapcol,job->host,sid)) {
               sbStrCopy(job->newsid[job->nnew++],sid,BNSP_IDSZ);
               g->nfetched++;
               free(buf);
          }
          else {
               free(buf);
               g->nbad++;
               sbStrCopy(g->err,"Could not write to the SNBETNWS\\IN folder",
                         sizeof(g->err));
               break;              /* disk trouble: try again later       */
          }
     }
     g->newnext=n;
     return(TRUE);
}

BOOL                               /*   TRUE if the article was spooled    */
bnwSpoolArticle(                   /* decode one article into the spool    */
const CHAR *spooldir,
const CHAR *group,
BNARTNUM artnum,
const CHAR *raw,
size_t len,
BOOL truncated,
INT wrapcol,
const CHAR *host,
CHAR *sid)
{
     IEMAIL m;
     BNRDY r;
     size_t parselen=len;
     BOOL ok;

     /*
      * A truncated article keeps only its header: half a MIME body would
      * decode into garbage.  The BBS shows a "too large" notice instead.
      */
     if (truncated) {
          const CHAR *end=strstr(raw,"\r\n\r\n");
          parselen=(end != NULL) ? (size_t)(end-raw)+4 : len;
     }
     if (!iemailParse(raw,parselen,wrapcol,&m)) {
          return(FALSE);
     }
     memset(&r,0,sizeof(r));
     sbStrCopy(r.group,group,sizeof(r.group));
     r.artnum=artnum;
     if (m.msgid[0] != '\0') {
          sbStrCopy(r.msgid,m.msgid,sizeof(r.msgid));
     }
     else {
          /* no Message-ID (should never happen): make a stable one up */
          _snprintf(r.msgid,sizeof(r.msgid)-1,"%I64d.%s@%s",artnum,group,host);
     }
     r.npar=bnartRefs(m.header,strlen(m.header),r.root,r.parents);
     sbStrCopy(r.from,m.from,sizeof(r.from));
     sbStrCopy(r.fromname,m.fromname,sizeof(r.fromname));
     sbStrCopy(r.replyto,m.replyto,sizeof(r.replyto));
     sbStrCopy(r.subject,m.subject,sizeof(r.subject));
     sbStrCopy(r.date,m.date,sizeof(r.date));
     r.size=(LONG)len;
     r.oversize=truncated;
     r.natt=m.natt+m.dropped;
     ok=bnspoolWrite(spooldir,&r,truncated ? "" : m.text,
                     truncated ? 0 : m.textlen);
     if (ok) {
          sbStrCopy(sid,r.sid,BNSP_IDSZ);
     }
     iemailFree(&m);
     return(ok);
}

/*--- helpers ------------------------------------------------------------*/

static VOID
failposts(                         /* posts not yet tried: try again later */
BNJOB *job,
const CHAR *why)
{
     INT i;

     for (i=0 ; i < job->npost ; i++) {
          if (job->post[i].status == BNP_NOTTRIED) {
               job->post[i].status=BNP_TEMP;
               sbStrCopy(job->post[i].text,why,sizeof(job->post[i].text));
          }
     }
}

static BOOL                        /*   TRUE if the connection is gone     */
lost(
BNNNTP *s)
{
     return(s->net.skt == INVALID_SOCKET || s->code < 0);
}

static VOID
logline(                           /* one transcript line into the log     */
VOID *ctx,
const CHAR *line)
{
     LOGCTX *lc=(LOGCTX *)ctx;

     if (lc->fp != NULL) {
          fprintf(lc->fp,"  %s\n",line);
     }
}
