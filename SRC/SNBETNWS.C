/***************************************************************************
 *                                                                         *
 *   SNBETNWS.C                                                            *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   A two-way gateway between Usenet newsgroups and the BBS forums.      *
 *                                                                         *
 *   How it fits into the BBS                                              *
 *   ------------------------                                              *
 *   The Sysop links newsgroups to forums in this module's Sysop menu.   *
 *   For each link:                                                        *
 *     import  new articles in the newsgroup are downloaded from a news   *
 *             server (NNTP, with a login if the server needs one) and    *
 *             put into the forum as ordinary forum messages, threaded    *
 *             the way they were on Usenet;                                *
 *     export  messages users write in the forum are posted to the        *
 *             newsgroup.                                                  *
 *   Messages this module imports are never exported again, and articles *
 *   this module posted are recognized when they come back from the news  *
 *   server, so nothing loops.                                             *
 *                                                                         *
 *   Threads                                                               *
 *   -------                                                               *
 *   Everything in this file runs on the BBS's single main thread and     *
 *   only does quick work: small files, Btrieve records, GME imports a    *
 *   few at a time.  The slow part (DNS, connecting, waiting for the news *
 *   server) runs on the worker thread in BNWORK.C, which never calls the  *
 *   BBS.  A timer (rtkick) runs every second: it collects a finished     *
 *   news session, starts the next one when it is due, and imports a few  *
 *   downloaded articles.                                                  *
 *                                                                         *
 *   Files (under the BBS directory)                                      *
 *   -------------------------------                                      *
 *     SNBETNWS.DAT   settings record + one record per newsgroup link     *
 *     SNBETMID.DAT   Message-ID cross-reference: which forum message is  *
 *                    which Usenet article (threading, and no duplicates) *
 *     SNBETNWS\IN\   downloaded articles waiting to be imported          *
 *     SNBETNWS\OUT\  forum messages waiting to be posted                 *
 *     SNBETNWS.LOG   optional transcript of news server sessions         *
 *                                                                         *
 *   Settings                                                              *
 *   --------                                                              *
 *   All settings are changed online by the Sysop from the module's menu  *
 *   and apply at once.  There are no CNF options.                        *
 *                                                                         *
 ***************************************************************************/

#include "BNWORK.H"                /* worker (includes Winsock first)      */
#include "BNART.H"
#include "SBMAIL.H"
#include "SBUTIL.H"
#include "BNVER.H"
#include "gcomm.h"
#include "majorbbs.h"
#include "gme.h"
#include "smtpexp.h"
#include "alias.h"
#include "SNBETNWS.H"
#include <stddef.h>
#include <ctype.h>
#include <time.h>

/* ======================================================================== */
/*   Records in SNBETNWS.DAT                                                 */
/* ======================================================================== */

#define RECKEYSZ     96            /* key: "!SETTINGS" or a newsgroup name */
#define CFGKEY       "!SETTINGS"   /* key of the settings record (a '!'    */
                                   /* cannot start a newsgroup name)       */
#define CFGVER       1             /* settings record layout version       */
#define GRPVER       1             /* newsgroup record layout version      */
#define MAXGRPS      BNW_MAXGRP    /* newsgroup links                      */

/*
 * Both record kinds are packed to 1-byte alignment so the layout never
 * depends on compiler settings, and kept at exactly 1024 bytes: new
 * fields are carved out of spare[] so existing files stay readable (zero
 * bytes must always mean "old behaviour").
 */
#pragma pack(push,1)
struct bncfg {                     /* the settings                         */
     CHAR recid[RECKEYSZ];         /*   key: CFGKEY                        */
     CHAR version;                 /*   CFGVER                             */
     CHAR enabled;                 /*   'Y' = gateway on                   */
     CHAR security;                /*   BNSEC_xxx                          */
     CHAR verify;                  /*   'Y' = check server certificates    */
     CHAR auditok;                 /*   'Y' = audit normal activity        */
     CHAR auditbad;                /*   'Y' = audit errors                 */
     CHAR translog;                /*   'Y' = write SNBETNWS.LOG           */
     CHAR showhdr;                 /*   'Y' = sender lines atop articles   */
     INT port;                     /*   news server port                   */
     INT pollmin;                  /*   minutes between checks             */
     INT timeout;                  /*   seconds to wait for each step      */
     INT backfill;                 /*   articles to read for a new group   */
     INT maxper;                   /*   most articles per group per check  */
     INT maxkb;                    /*   largest article imported in full   */
     INT wrapcol;                  /*   word-wrap column (0 = off)         */
     INT retrymin;                 /*   minutes between posting attempts   */
     INT giveuphrs;                /*   hours before a post is given up    */
     INT keepdays;                 /*   days Message-IDs are remembered    */
     CHAR host[64];                /*   news server                        */
     CHAR user[64];                /*   login name ("" = none)             */
     CHAR pass[64];                /*   password                           */
     CHAR fromname[64];            /*   From name template                 */
     CHAR domain[64];              /*   Message-ID host ("" = mail host)   */
     CHAR org[96];                 /*   Organization ("" = none)           */
     CHAR postkey[KEYSIZ];         /*   key needed to post ("" = anyone)   */
     CHAR spare[448];              /*   room for future settings           */
};

struct bngrp {                     /* one newsgroup link                   */
     CHAR recid[RECKEYSZ];         /*   key: newsgroup name, lower case    */
     CHAR version;                 /*   GRPVER                             */
     CHAR kind;                    /*   'G'                                */
     CHAR name[RECKEYSZ];          /*   newsgroup name as entered          */
     CHAR forum[FORNSZ];           /*   forum name                         */
     CHAR doimp;                   /*   'Y' = import articles              */
     CHAR doexp;                   /*   'Y' = export forum messages        */
     __int64 next;                 /*   next article number to read        */
                                   /*   (0 = start fresh with backfill)    */
     LONG nimp;                    /*   articles imported, all time        */
     LONG nexp;                    /*   messages posted, all time          */
     __int64 lastok;               /*   last time the group was read       */
     CHAR lasterr[96];             /*   last problem ("" = none)           */
     CHAR spare[692];              /*   room for future fields             */
};

/*
 * SNBETMID.DAT: one record per forum message that is also a Usenet
 * article (imported, or posted from here).  512 bytes.
 *   key 0  forum + Message-ID   unique: an article is in a forum once.
 *          Only the first MIDKEYSZ characters of the Message-ID are in
 *          the key: Btrieve needs eight keys to fit in one page (status
 *          29 on create otherwise), and real Message-IDs are far shorter.
 *   key 1  GME global id        (gsys,gmsg): Message-ID of a forum message
 *   key 2  time added           to forget old records
 */
struct bnmid {
     USHORT forum;                 /*   forum it is in                     */
     CHAR mid[BN_MIDSZ];           /*   Message-ID without < >             */
     LONG gsys;                    /*   GME global id: system part         */
     LONG gmsg;                    /*   GME global id: message part        */
     LONG added;                   /*   time() when recorded               */
     CHAR root[BN_MIDSZ];          /*   first article of its thread ("")   */
     CHAR spare[2];
};
#pragma pack(pop)

/* compile-time checks: the build fails here if a record size drifts */
typedef CHAR bncfg_is_1024[(sizeof(struct bncfg) == 1024) ? 1 : -1];
typedef CHAR bngrp_is_1024[(sizeof(struct bngrp) == 1024) ? 1 : -1];
typedef CHAR bnmid_is_512[(sizeof(struct bnmid) == 512) ? 1 : -1];

static struct bncfg cfg;           /* current settings                     */
static struct bngrp grps[MAXGRPS]; /* newsgroup links                      */
static INT ngrps=0;
static DFAFILE *cfgbb=NULL;        /* SNBETNWS.DAT                         */
static DFAFILE *midbb=NULL;        /* SNBETMID.DAT                         */

static struct dfaSegSpec cfgsegs[1]={   /* SNBETNWS.DAT key 0: recid       */
     {0,RECKEYSZ,DFAST_STRING,0,'\0'}
};
static struct dfaKeySpec cfgkeys[1]={{0,1,cfgsegs}};

#define MIDKEYSZ     200           /* Message-ID characters in key 0       */

static struct dfaSegSpec midseg0[2]={   /* SNBETMID.DAT key 0: forum+mid   */
     {offsetof(struct bnmid,forum),2,DFAST_BINARY,0,'\0'},
     {offsetof(struct bnmid,mid),MIDKEYSZ,DFAST_STRING,0,'\0'}
};
static struct dfaSegSpec midseg1[2]={   /* key 1: GME global id            */
     {offsetof(struct bnmid,gsys),4,DFAST_INTEGER,0,'\0'},
     {offsetof(struct bnmid,gmsg),4,DFAST_INTEGER,0,'\0'}
};
static struct dfaSegSpec midseg2[1]={   /* key 2: time added               */
     {offsetof(struct bnmid,added),4,DFAST_INTEGER,0,'\0'}
};
static struct dfaKeySpec midkeys[3]={
     {0,2,midseg0},
     {DFAKF_DUPLICATE,2,midseg1},
     {DFAKF_DUPLICATE,1,midseg2}
};

#pragma pack(push,1)
struct gidkey {                    /* SNBETMID.DAT key 1 value             */
     LONG gsys;
     LONG gmsg;
};
#pragma pack(pop)

/* ======================================================================== */
/*   Outgoing posts and incoming articles                                    */
/* ======================================================================== */

typedef struct oqitem {            /* one forum message waiting to go out  */
     LONG qid;                     /*   id (also the file name)            */
     time_t queued;                /*   when it was written                */
     time_t nexttry;               /*   earliest time for the next attempt */
     INT tries;                    /*   attempts so far                    */
     BOOL inflight;                /*   currently with the worker?         */
     CHAR author[UIDSIZ];          /*   who wrote it                       */
     CHAR groups[256];             /*   newsgroup(s) it goes to            */
     CHAR topic[TPCSIZ];           /*   subject (CP437)                    */
     CHAR msgid[BN_MIDSZ];         /*   its Message-ID                     */
     CHAR lasterr[BNW_ERRSZ];      /*   last failure, for the Sysop        */
} OQITEM;

static OQITEM *outq=NULL;          /* waiting to be posted                 */
static INT noutq=0;
static INT outroom=0;
static LONG lastqid=0;

static CHAR (*pend)[BNSP_IDSZ]=NULL;  /* downloaded, oldest first          */
static INT npend=0;
static INT pendroom=0;

#define MAXPARTS     8             /* most BBS messages per article        */

typedef struct cur {               /* the article being imported           */
     BOOL active;                  /*   an article is loaded               */
     BNRDY r;                      /*   its spool details                  */
     INT gidx;                     /*   newsgroup link it came through     */
     USHORT fid;                   /*   forum it goes to                   */
     CHAR *text;                   /*   whole BBS text (CR line ends)      */
     size_t textlen;
     size_t offs[MAXPARTS+1];      /*   where each part starts             */
     INT nparts;
     INT part;                     /*   part being imported now            */
     BOOL again;                   /*   impmsg() asked to be called again  */
     struct message msg;           /*   message being imported             */
     struct globid parent;         /*   forum message it replies to (or 0) */
     struct globid first;          /*   part 1, once imported              */
     USHORT crdate,crtime;         /*   article date (0 = now)             */
     BOOL imported;                /*   part 1 went in                     */
} CUR;

static CUR cur;
static CHAR *partbuf=NULL;         /* text of the part being imported      */
static CHAR *workarea=NULL;        /* GME work area for impmsg()           */
static BNJOB *jobbuf=NULL;         /* job going to / coming from worker    */

#define TICKSECS     1             /* seconds between timer runs           */
#define STEPSPERTICK 8             /* import steps per timer run           */
#define PURGEPERTICK 25            /* old Message-IDs forgotten per run    */

static CHAR bbsdir[MAX_PATH];      /* BBS directory, captured at startup   */
static CHAR indir[MAX_PATH];       /* incoming spool, with trailing '\'    */
static CHAR outdir[MAX_PATH];      /* outgoing queue, with trailing '\'    */
static CHAR logpath[MAX_PATH];     /* transcript log file                  */

static BOOL running=FALSE;         /* initialized and not shut down        */
static BOOL checknow=FALSE;        /* Sysop asked for a check              */
static BOOL busy=FALSE;            /* a session is with the worker         */
static BOOL busyfetch=FALSE;       /*   ...and it reads the groups         */
static time_t nextpoll=0;          /* when the next check is due           */
static time_t lastpoll=0;          /* when the last check finished         */
static BOOL lastok=TRUE;           /* did it work?                         */
static CHAR lasterr[SBNET_ERRSZ];  /* if not, why                          */
static time_t lasterraudit=0;      /* last server-error Audit Trail line   */
static INT lastnew=0;              /* articles the last check downloaded   */
static time_t nextpurge=0;         /* when to look for old Message-IDs     */
static LONG nimported=0;           /* articles imported since startup      */
static LONG nposted=0;             /* messages posted since startup        */

/* ======================================================================== */
/*   Text variables (named values usable in SNBETNWS.MSG text)              */
/* ======================================================================== */

static CHAR tvversion[32];
static CHAR tvstatus[128];
static CHAR tvwait[64];
static CHAR tvpage[48];
static CHAR tvrownum[8];
static CHAR tvrowname[48];
static CHAR tvrowval[112];
static CHAR tvhint[96];
static CHAR tvchnum[8];
static CHAR tvchname[16];
static CHAR tvchdesc[64];
static CHAR tvgnum[8];
static CHAR tvgname[RECKEYSZ];
static CHAR tvgforum[FORNSZ];
static CHAR tvgimp[8];
static CHAR tvgexp[8];
static CHAR tvgstat[48];
static CHAR tvqgrp[64];
static CHAR tvqsubj[TPCSIZ];
static CHAR tvqage[24];
static CHAR tvqtries[12];
static CHAR tvqerr[80];
static CHAR tvfrom[400];
static CHAR tvdate[64];
static CHAR tvpart[8];
static CHAR tvnparts[8];
static CHAR tvsize[16];
static CHAR tvlimit[16];
static CHAR tvnatt[8];
static CHAR tvrjgrp[256];
static CHAR tvrjsubj[TPCSIZ];
static CHAR tvrjwhy[BNW_ERRSZ+64];

static CHAR *tv_version(VOID)  { return(tvversion); }
static CHAR *tv_status(VOID)   { return(tvstatus); }
static CHAR *tv_wait(VOID)     { return(tvwait); }
static CHAR *tv_page(VOID)     { return(tvpage); }
static CHAR *tv_rownum(VOID)   { return(tvrownum); }
static CHAR *tv_rowname(VOID)  { return(tvrowname); }
static CHAR *tv_rowval(VOID)   { return(tvrowval); }
static CHAR *tv_hint(VOID)     { return(tvhint); }
static CHAR *tv_chnum(VOID)    { return(tvchnum); }
static CHAR *tv_chname(VOID)   { return(tvchname); }
static CHAR *tv_chdesc(VOID)   { return(tvchdesc); }
static CHAR *tv_gnum(VOID)     { return(tvgnum); }
static CHAR *tv_gname(VOID)    { return(tvgname); }
static CHAR *tv_gforum(VOID)   { return(tvgforum); }
static CHAR *tv_gimp(VOID)     { return(tvgimp); }
static CHAR *tv_gexp(VOID)     { return(tvgexp); }
static CHAR *tv_gstat(VOID)    { return(tvgstat); }
static CHAR *tv_qgrp(VOID)     { return(tvqgrp); }
static CHAR *tv_qsubj(VOID)    { return(tvqsubj); }
static CHAR *tv_qage(VOID)     { return(tvqage); }
static CHAR *tv_qtries(VOID)   { return(tvqtries); }
static CHAR *tv_qerr(VOID)     { return(tvqerr); }
static CHAR *tv_from(VOID)     { return(tvfrom); }
static CHAR *tv_date(VOID)     { return(tvdate); }
static CHAR *tv_part(VOID)     { return(tvpart); }
static CHAR *tv_nparts(VOID)   { return(tvnparts); }
static CHAR *tv_size(VOID)     { return(tvsize); }
static CHAR *tv_limit(VOID)    { return(tvlimit); }
static CHAR *tv_natt(VOID)     { return(tvnatt); }
static CHAR *tv_bbs(VOID)      { return(bbsttl); }
static CHAR *tv_rjgrp(VOID)    { return(tvrjgrp); }
static CHAR *tv_rjsubj(VOID)   { return(tvrjsubj); }
static CHAR *tv_rjwhy(VOID)    { return(tvrjwhy); }

/* ======================================================================== */
/*   Sysop editor: the list of settings                                     */
/* ======================================================================== */

#define FT_YN        0             /* yes/no                               */
#define FT_NUM       1             /* number in a range                    */
#define FT_STR       2             /* text                                 */
#define FT_SECRET    3             /* password: never displayed            */
#define FT_SEC       4             /* NONE, STARTTLS or TLS                */
#define FT_WRAP      5             /* 0 (off) or a column 40-132           */
#define FT_KEY       6             /* a key name (one word)                */
#define FT_GNAME     7             /* a newsgroup name                     */
#define FT_FORUM     8             /* an existing forum                    */
#define FT_ART       9             /* an article number (0 or more)        */
#define FT_BACK      10            /* 0-max, or ALL (BNW_BACKALL)          */

#define REC_CFG      0             /* field is in the settings record      */
#define REC_GRP      1             /* field is in a newsgroup record       */

typedef struct field {             /* one editable setting                 */
     const CHAR *name;             /*   label on the screen                */
     INT type;                     /*   FT_xxx                             */
     INT rec;                      /*   REC_xxx                            */
     size_t off;                   /*   offset in its record               */
     size_t size;                  /*   size of a text field               */
     INT min,max;                  /*   range of a number                  */
     const CHAR *hint;             /*   what may be entered                */
     INT help;                     /*   help message (? at its prompt)     */
} FIELD;

#define F(n,t,m,lo,hi,h,hm) {n,t,REC_CFG,offsetof(struct bncfg,m), \
                             sizeof(((struct bncfg *)0)->m),lo,hi,h,hm}
#define G(n,t,m,lo,hi,h,hm) {n,t,REC_GRP,offsetof(struct bngrp,m), \
                             sizeof(((struct bngrp *)0)->m),lo,hi,h,hm}

/*
 * The settings, in the order they are numbered and shown.  The number a
 * Sysop types is the position here; the record layout does not depend on
 * it, so settings can be renumbered freely.
 */
static const FIELD fields[]={
     F("Gateway enabled",          FT_YN,    enabled,  0,0,
       "YES or NO",HENABLE),
     F("News server",              FT_STR,   host,     0,0,
       "host name, e.g. news.example.com",HHOST),
     F("News server port",         FT_NUM,   port,     1,65535,
       "1-65535 (119 plain/STARTTLS, 563 TLS)",HPORT),
     F("Connection security",      FT_SEC,   security, 0,0,
       "NONE, STARTTLS or TLS",HSEC),
     F("Verify TLS certificate",   FT_YN,    verify,   0,0,
       "YES (recommended) or NO",HVERIFY),
     F("Login name",               FT_STR,   user,     0,0,
       "login name, or - for none",HUSER),
     F("Password",                 FT_SECRET,pass,     0,0,
       "password, or - to clear",HPASS),
     F("Check every (minutes)",    FT_NUM,   pollmin,  1,1440,
       "1-1440",HPOLL),
     F("Timeout (seconds)",        FT_NUM,   timeout,  10,300,
       "10-300",HTMOUT),
     F("Articles for a new group", FT_BACK,  backfill, 0,9999999,
       "0-9999999, or ALL (0 = only articles from now on)",HBACK),
     F("Most articles per check",  FT_NUM,   maxper,   1,1000,
       "1-1000 per newsgroup",HMAXPER),
     F("Largest article (KB)",     FT_NUM,   maxkb,    4,1024,
       "4-1024",HMAXKB),
     F("From name",                FT_STR,   fromname, 0,0,
       "display name; {USER} = User-ID, {BBS} = BBS name",HFNAME),
     F("Message-ID host name",     FT_STR,   domain,   0,0,
       "e.g. bbs.example.com, or - for the BBS mail host name",HDOMAIN),
     F("Organization",             FT_STR,   org,      0,0,
       "one line, or - for none",HORG),
     F("Key needed to post",       FT_KEY,   postkey,  0,0,
       "a key name, or - for anyone who can write in the forum",HPOSTKEY),
     F("Retry interval (minutes)", FT_NUM,   retrymin, 1,1440,
       "1-1440",HRETRY),
     F("Give up after (hours)",    FT_NUM,   giveuphrs,1,720,
       "1-720",HGIVEUP),
     F("Sender line in messages",  FT_YN,    showhdr,  0,0,
       "YES or NO",HSHOWHDR),
     F("Word-wrap column",         FT_WRAP,  wrapcol,  0,132,
       "0 (off) or 40-132",HWRAP),
     F("Keep Message-IDs (days)",  FT_NUM,   keepdays, 7,3650,
       "7-3650",HKEEP),
     F("Audit normal activity",    FT_YN,    auditok,  0,0,
       "YES or NO",HAUDOK),
     F("Audit errors",             FT_YN,    auditbad, 0,0,
       "YES or NO",HAUDBAD),
     F("Transcript log file",      FT_YN,    translog, 0,0,
       "YES or NO (writes SNBETNWS.LOG)",HTRANS)
};
#define NFIELDS ((INT)(sizeof(fields)/sizeof(fields[0])))

static const FIELD gfields[]={     /* one newsgroup link's settings        */
     G("Newsgroup",                FT_GNAME, name,     0,0,
       "newsgroup name, e.g. alt.bbs.synchronet",HGNAME),
     G("Forum",                    FT_FORUM, forum,    0,0,
       "the name of an existing forum",HGFORUM),
     G("Import articles",          FT_YN,    doimp,    0,0,
       "YES or NO",HGIMP),
     G("Export messages",          FT_YN,    doexp,    0,0,
       "YES or NO",HGEXP),
     G("Next article number",      FT_ART,   next,     0,0,
       "a number, or 0 to start over with the newest articles",HGNEXT)
};
#define NGFIELDS ((INT)(sizeof(gfields)/sizeof(gfields[0])))

/*
 * The settings are shown on two pages so each screen fits a 24-line
 * terminal.  Settings keep their numbers on both pages, so any number can
 * be typed from either page.
 */
typedef struct page {
     const CHAR *title;
     const INT *flds;
     INT n;
} PAGE;

static const INT pg1flds[]={0,1,2,3,4,5,6,7,8,9,10,11};
static const INT pg2flds[]={12,13,14,15,16,17,18,19,20,21,22,23};

static const PAGE pages[]={
     {"News server",         pg1flds,(INT)(sizeof(pg1flds)/sizeof(INT))},
     {"Posting and logging", pg2flds,(INT)(sizeof(pg2flds)/sizeof(INT))}
};
#define NPAGES ((INT)(sizeof(pages)/sizeof(pages[0])))

#define ROWVALW      40            /* widest value shown in a settings row */
                                   /* (keeps rows clear of the host's      */
                                   /* automatic wrap near column 80)       */
#define GROWS        14            /* newsgroups listed per screen         */

                                   /* editor states (usrptr->substt)       */
#define ST_MENU      1             /*   at the settings menu               */
#define ST_VALUE     2             /*   entering a new value               */
#define ST_CHOICE    3             /*   picking one of a setting's choices */
#define ST_GROUPS    4             /*   at the newsgroup list              */
#define ST_GADDNAME  5             /*   adding: newsgroup name             */
#define ST_GADDFOR   6             /*   adding: forum                      */
#define ST_GEDIT     7             /*   at one newsgroup's settings        */
#define ST_GVALUE    8             /*   entering a newsgroup setting       */
#define ST_GCHOICE   9             /*   picking a newsgroup setting choice */
#define ST_GDELASK   10            /*   confirming a removal               */

static INT editfld[MAXNTERM];      /* setting being edited, per channel    */
static INT curpage[MAXNTERM];      /* settings page shown, per channel     */
static INT curgrp[MAXNTERM];       /* newsgroup being edited, per channel  */
static INT grptop[MAXNTERM];       /* first newsgroup listed, per channel  */
static CHAR addname[MAXNTERM][RECKEYSZ]; /* newsgroup being added          */
static GBOOL menushown;            /* screen (with prompt) just printed?   */

/* ======================================================================== */
/*   Forward declarations                                                    */
/* ======================================================================== */

static VOID tick(VOID);
static VOID dispatch(BOOL dofetch);
static VOID collect(VOID);
static VOID postresult(const BNWPOST *p);
static VOID importsome(VOID);
static BOOL loadnext(VOID);
static BOOL step(VOID);
static VOID finishcur(VOID);
static VOID dropcur(VOID);
static VOID buildtext(const CHAR *body, size_t bodylen);
static VOID addtext(const CHAR *s);
static VOID splitparts(VOID);
static VOID buildmsg(VOID);
static VOID articledate(const CHAR *date, USHORT *d, USHORT *t);
static const CHAR *msgtext(INT msgno);
static VOID loadspool(VOID);
static VOID addpend(const CHAR *sid);
static INT pendcmp(const VOID *a, const VOID *b);

static VOID bnnewmsg(INT newtype, const struct message *msg,
                       const CHAR *text);
static VOID queuepost(const struct message *msg, const CHAR *text,
                      const CHAR *groups);
static VOID rejectnotice(const OQITEM *q, const CHAR *why);
static VOID loadoutq(VOID);
static GBOOL writeque(const OQITEM *q);
static GBOOL readque(const CHAR *path, OQITEM *q);
static VOID oqpath(CHAR *dst, LONG qid, const CHAR *ext);
static VOID oqdelete(INT idx);
static INT oqfind(LONG qid);
static LONG newqid(VOID);
static VOID cvtfrom(CHAR *dst, const CHAR *src);
static VOID fillname(CHAR *dst, size_t dstsz, const CHAR *tmpl,
                     const CHAR *userid);
static const CHAR *ourhost(VOID);

static GBOOL midfind(const CHAR *mid, USHORT forum, struct bnmid *out);
static GBOOL midbygid(const struct globid *g, struct bnmid *out);
static VOID midadd(const CHAR *mid, USHORT forum, const struct globid *g,
                   const CHAR *root);
static VOID midpurge(VOID);

static VOID cfgdefaults(VOID);
static VOID cfgload(VOID);
static GBOOL cfgsave(VOID);
static VOID grpload(VOID);
static GBOOL grpsave(INT i, const CHAR *oldkey);
static GBOOL grpremove(INT i);
static INT grpfind(const CHAR *name);
static VOID grpkey(CHAR *dst, const CHAR *name);
static GBOOL configured(VOID);
static const CHAR *missing(VOID);

static VOID audit(const CHAR *brief, const CHAR *fmt, ...);
static VOID setstatus(VOID);
static VOID showmenu(VOID);
static VOID showgroups(VOID);
static VOID showgroup(VOID);
static INT pageof(INT fld);
static INT nchoices(const FIELD *f);
static const CHAR *choicekey(const FIELD *f, INT i);
static const CHAR *choicedesc(const FIELD *f, INT i);
static VOID showchoices(const FIELD *f);
static CHAR *fieldbase(const FIELD *f);
static VOID fieldvalue(const FIELD *f, CHAR *dst, size_t dstsz);
static GBOOL setfield(const FIELD *f, const CHAR *input);
static VOID applyvalue(const FIELD *f, const CHAR *value);
static const CHAR *unused(const FIELD *f);
static VOID showqueue(VOID);
static GBOOL validgroup(const CHAR *name);
static VOID sanitize(CHAR *s);
static VOID startedit(const FIELD *f, INT num, INT valstate, INT chstate);

GBOOL bnentry(VOID);
VOID bnshutdown(VOID);
VOID bnhangup(VOID);

/* ======================================================================== */
/*   Module registration                                                     */
/* ======================================================================== */

INT bnstate;                       /* our module's state number            */
static HMCVFILE bnmb=NULL;         /* SNBETNWS.MCV                         */

struct module SNBETNWS={           /* module interface block               */
     "",                           /*   name (filled in from the .MDF)     */
     NULL,                         /*   user logon routine                 */
     bnentry,                      /*   input routine (Sysop menu)         */
     dfsthn,                       /*   status-input routine (default)     */
     NULL,                         /*   "injoth" routine                   */
     NULL,                         /*   user logoff routine                */
     bnhangup,                     /*   hangup routine                     */
     NULL,                         /*   nightly cleanup routine            */
     NULL,                         /*   delete-account routine             */
     bnshutdown                    /*   system shutdown routine            */
};

VOID EXPORT
init__snbetnws(VOID)               /* module initialization                */
{
     CHAR msg[120];
     CHAR dir[MAX_PATH];

     stzcpy(SNBETNWS.descrp,gmdnam("SNBETNWS.MDF"),MNMSIZ);
     bnstate=register_module(&SNBETNWS);
     bnmb=opnmsg("SNBETNWS.MCV");

     /* the working directory is the BBS directory while modules load */
     GetCurrentDirectoryA(sizeof(bbsdir),bbsdir);
     _snprintf(dir,sizeof(dir)-1,"%s\\SNBETNWS",bbsdir);
     dir[sizeof(dir)-1]='\0';
     CreateDirectoryA(dir,NULL);
     _snprintf(indir,sizeof(indir)-1,"%s\\IN\\",dir);
     _snprintf(outdir,sizeof(outdir)-1,"%s\\OUT\\",dir);
     _snprintf(logpath,sizeof(logpath)-1,"%s\\SNBETNWS.LOG",bbsdir);
     CreateDirectoryA(indir,NULL);
     CreateDirectoryA(outdir,NULL);

     stzcpy(tvversion,BNVER_STR,sizeof(tvversion));
     register_textvar("SNBN_VERSION",tv_version);
     register_textvar("SNBN_STATUS",tv_status);
     register_textvar("SNBN_WAIT",tv_wait);
     register_textvar("SNBN_PAGE",tv_page);
     register_textvar("SNBN_ROWNUM",tv_rownum);
     register_textvar("SNBN_ROWNAME",tv_rowname);
     register_textvar("SNBN_ROWVAL",tv_rowval);
     register_textvar("SNBN_HINT",tv_hint);
     register_textvar("SNBN_CHNUM",tv_chnum);
     register_textvar("SNBN_CHNAME",tv_chname);
     register_textvar("SNBN_CHDESC",tv_chdesc);
     register_textvar("SNBN_GNUM",tv_gnum);
     register_textvar("SNBN_GNAME",tv_gname);
     register_textvar("SNBN_GFORUM",tv_gforum);
     register_textvar("SNBN_GIMP",tv_gimp);
     register_textvar("SNBN_GEXP",tv_gexp);
     register_textvar("SNBN_GSTAT",tv_gstat);
     register_textvar("SNBN_QGRP",tv_qgrp);
     register_textvar("SNBN_QSUBJ",tv_qsubj);
     register_textvar("SNBN_QAGE",tv_qage);
     register_textvar("SNBN_QTRIES",tv_qtries);
     register_textvar("SNBN_QERR",tv_qerr);
     register_textvar("SNBN_FROM",tv_from);
     register_textvar("SNBN_DATE",tv_date);
     register_textvar("SNBN_PART",tv_part);
     register_textvar("SNBN_NPARTS",tv_nparts);
     register_textvar("SNBN_SIZE",tv_size);
     register_textvar("SNBN_LIMIT",tv_limit);
     register_textvar("SNBN_NATT",tv_natt);
     register_textvar("SNBN_BBS",tv_bbs);
     register_textvar("SNBN_RJGRP",tv_rjgrp);
     register_textvar("SNBN_RJSUBJ",tv_rjsubj);
     register_textvar("SNBN_RJWHY",tv_rjwhy);

     /*
      * Data files: created on first run only (asking Btrieve to create a
      * file that exists logs a harmless but alarming error 59).
      */
     if (GetFileAttributesA("SNBETNWS.DAT") == INVALID_FILE_ATTRIBUTES) {
          dfaCreateSpec("SNBETNWS.DAT",FALSE,sizeof(struct bncfg),2048,0,0,1,
                        cfgkeys,NULL);
     }
     if (GetFileAttributesA("SNBETMID.DAT") == INVALID_FILE_ATTRIBUTES) {
          dfaCreateSpec("SNBETMID.DAT",FALSE,sizeof(struct bnmid),2048,0,0,3,
                        midkeys,NULL);
     }
     cfgbb=dfaOpen("SNBETNWS.DAT",(USHORT)sizeof(struct bncfg),NULL);
     midbb=dfaOpen("SNBETMID.DAT",(USHORT)sizeof(struct bnmid),NULL);
     cfgload();
     grpload();

     workarea=(CHAR *)alczer(GMEWRKSZ);
     partbuf=(CHAR *)alczer(TXTLEN);
     jobbuf=(BNJOB *)alczer(sizeof(BNJOB));
     setmem((CHAR *)&cur,sizeof(cur),0);
     loadspool();
     loadoutq();

     if (!bnwStart()) {
          audit("NEWSGROUPS NOT STARTED","Could not start the news worker "
                "thread");
          return;
     }
     /* forum messages users write: candidates for posting to Usenet */
     hook_gme(GMEHOOK_NOT_NEWMSG,(voidfunc)bnnewmsg);
     running=TRUE;
     nextpoll=time(NULL)+30;       /* first check soon after startup      */
     rtkick(TICKSECS,tick);

     _snprintf(msg,sizeof(msg)-1,"v%s loaded; gateway %s, %d group(s), %d to "
               "import, %d to post",BNVER_STR,cfg.enabled == 'Y' ? "ON"
               : "OFF",ngrps,npend,noutq);
     msg[sizeof(msg)-1]='\0';
     audit(BNPRODUCT,"%s",msg);
}

/* ======================================================================== */
/*   The timer                                                               */
/* ======================================================================== */

static VOID
tick(VOID)
{
     time_t now;
     INT i;

     if (!running) {
          return;                  /* shut down: let the timer lapse      */
     }
     now=time(NULL);
     collect();
     if (!busy && configured() && !bnwBusy()) {
          BOOL duepoll=FALSE,duepost=FALSE;

          if (checknow || (cfg.enabled == 'Y' && now >= nextpoll)) {
               for (i=0 ; i < ngrps ; i++) {
                    if (grps[i].doimp == 'Y') {
                         duepoll=TRUE;
                    }
               }
               if (!duepoll) {
                    nextpoll=now+(time_t)cfg.pollmin*60;
               }
          }
          if (cfg.enabled == 'Y' || checknow) {
               for (i=0 ; i < noutq ; i++) {
                    if (!outq[i].inflight && outq[i].nexttry <= now) {
                         duepost=TRUE;
                    }
               }
          }
          if (duepoll || duepost) {
               dispatch(duepoll);
          }
          checknow=FALSE;
     }
     importsome();
     if (now >= nextpurge) {
          midpurge();
     }
     rtkick(TICKSECS,tick);
}

/*
 * Start a news session.  The job carries copies of everything the worker
 * needs: the server settings, the newsgroups to read (with where each
 * left off) and the posts that are due.
 */
static VOID
dispatch(
BOOL dofetch)
{
     BNJOB *job=jobbuf;
     time_t now=time(NULL);
     INT i;

     setmem((CHAR *)job,sizeof(*job),0);
     stzcpy(job->host,cfg.host,sizeof(job->host));
     job->port=cfg.port;
     job->security=cfg.security;
     job->verify=(cfg.verify == 'Y');
     stzcpy(job->user,cfg.user,sizeof(job->user));
     stzcpy(job->pass,cfg.pass,sizeof(job->pass));
     job->timeout=cfg.timeout;
     stzcpy(job->spooldir,indir,sizeof(job->spooldir));
     if (cfg.translog == 'Y') {
          stzcpy(job->logfile,logpath,sizeof(job->logfile));
     }
     job->maxbytes=(LONG)cfg.maxkb*1024L;
     job->maxper=cfg.maxper;
     job->backfill=cfg.backfill;
     job->wrapcol=cfg.wrapcol;
     job->maxnew=BNW_MAXNEW;
     job->dofetch=dofetch;
     if (dofetch) {
          for (i=0 ; i < ngrps && job->ngrp < BNW_MAXGRP ; i++) {
               if (grps[i].doimp != 'Y') {
                    continue;
               }
               stzcpy(job->grp[job->ngrp].name,grps[i].name,BNSP_GROUPSZ);
               job->grp[job->ngrp].next=grps[i].next;
               job->ngrp++;
          }
     }
     for (i=0 ; i < noutq && job->npost < BNW_MAXPOST ; i++) {
          if (outq[i].inflight || outq[i].nexttry > now) {
               continue;
          }
          job->post[job->npost].qid=outq[i].qid;
          oqpath(job->post[job->npost].artfile,outq[i].qid,"ART");
          job->npost++;
     }
     if (bnwSubmit(job)) {
          busy=TRUE;
          busyfetch=dofetch;
          for (i=0 ; i < job->npost ; i++) {
               INT k=oqfind(job->post[i].qid);
               if (k >= 0) {
                    outq[k].inflight=TRUE;
                    outq[k].tries++;
               }
          }
     }
     bnwWipe(job);
}

/*
 * Take in a finished session: post outcomes, where each newsgroup now
 * stands, and the newly downloaded articles (which join the import line).
 */
static VOID
collect(VOID)
{
     BNJOB *job=jobbuf;
     time_t now;
     INT i,k;

     if (!bnwCollect(job)) {
          return;
     }
     bnwWipe(job);
     busy=FALSE;
     now=time(NULL);
     for (i=0 ; i < job->npost ; i++) {
          postresult(&job->post[i]);
     }
     if (job->dofetch) {
          lastpoll=now;
          nextpoll=now+(time_t)cfg.pollmin*60;
     }
     if (!job->ok) {
          stzcpy(lasterr,job->err,sizeof(lasterr));
          if (cfg.auditbad == 'Y' && (lastok || now-lasterraudit >= 3600)) {
               audit("NEWS SERVER PROBLEM","%.16s: %.90s",job->stage,job->err);
               lasterraudit=now;
          }
          lastok=FALSE;
          return;
     }
     if (!lastok && cfg.auditbad == 'Y') {
          audit("NEWS SERVER OK","The news server can be reached again");
     }
     lastok=TRUE;
     lasterr[0]='\0';
     if (!job->dofetch) {
          return;
     }
     lastnew=job->nnew;

     /* where each newsgroup now stands */
     for (i=0 ; i < job->ngrp ; i++) {
          BNWGRP *g=&job->grp[i];

          if (!g->tried || (k=grpfind(g->name)) < 0) {
               continue;
          }
          if (g->ok) {
               grps[k].next=g->newnext;
               grps[k].lastok=(__int64)now;
          }
          if (g->err[0] != '\0') {
               if (cfg.auditbad == 'Y' && strcmp(grps[k].lasterr,g->err) != 0) {
                    audit("NEWSGROUP PROBLEM","%.30s: %.80s",g->name,g->err);
               }
               stzcpy(grps[k].lasterr,g->err,sizeof(grps[k].lasterr));
          }
          else {
               grps[k].lasterr[0]='\0';
          }
          if (g->renumbered && cfg.auditbad == 'Y') {
               audit("NEWSGROUP RENUMBERED","%.40s: server numbers went down; "
                     "restarted",g->name);
          }
          if (cfg.auditok == 'Y' && g->nfetched > 0) {
               audit("NEWSGROUP READ","%.40s: %d new article(s)",g->name,
                     g->nfetched);
          }
          grpsave(k,NULL);
     }

     /* newly downloaded articles join the import line */
     for (i=0 ; i < job->nnew ; i++) {
          addpend(job->newsid[i]);
     }
}

/*
 * One post's outcome: posted (forget it), refused for good (tell the
 * author), or failed for now (try again later, until the Sysop's
 * "give up after" time).
 */
static VOID
postresult(
const BNWPOST *p)
{
     INT k;
     OQITEM *q;
     CHAR why[BNW_ERRSZ+64];

     if ((k=oqfind(p->qid)) < 0) {
          return;
     }
     q=&outq[k];
     q->inflight=FALSE;
     if (p->status == BNP_OK) {
          CHAR list[sizeof(q->groups)],*g;

          nposted++;
          stzcpy(list,q->groups,sizeof(list));
          for (g=strtok(list,",") ; g != NULL ; g=strtok(NULL,",")) {
               INT gi=grpfind(g);
               if (gi >= 0) {
                    grps[gi].nexp++;
                    grpsave(gi,NULL);
               }
          }
          if (cfg.auditok == 'Y') {
               audit("NEWSGROUP POSTED","%.30s by %.29s",q->groups,q->author);
          }
          oqdelete(k);
          return;
     }
     if (p->status == BNP_NOTTRIED) {
          q->tries--;              /* never attempted: not a try          */
          return;
     }
     stzcpy(q->lasterr,p->text,sizeof(q->lasterr));
     if (p->status == BNP_PERM) {
          if (cfg.auditbad == 'Y') {
               audit("NEWSGROUP POST REFUSED","%.30s: %.80s",q->groups,p->text);
          }
          rejectnotice(q,p->text);
          oqdelete(k);
          return;
     }
     if (time(NULL)-q->queued >= (time_t)cfg.giveuphrs*3600) {
          _snprintf(why,sizeof(why)-1,"Could not be posted for %d hours. "
                    "Last error: %s",cfg.giveuphrs,p->text);
          why[sizeof(why)-1]='\0';
          if (cfg.auditbad == 'Y') {
               audit("NEWSGROUP POST GIVEN UP","%.30s by %.29s",q->groups,
                     q->author);
          }
          rejectnotice(q,why);
          oqdelete(k);
          return;
     }
     q->nexttry=time(NULL)+(time_t)cfg.retrymin*60;
     writeque(q);
}

/* ======================================================================== */
/*   Importing downloaded articles into the forums                          */
/* ======================================================================== */

/*
 * A few import steps per timer run, so a burst of articles never holds
 * up the board.  impmsg() may ask to be called again (GMEAGAIN); then the
 * rest waits for the next run.
 */
static VOID
importsome(VOID)
{
     INT n;

     for (n=0 ; n < STEPSPERTICK ; n++) {
          if (!cur.active && !loadnext()) {
               return;             /* nothing waiting                     */
          }
          if (!cur.active) {
               continue;           /* that one needed no import           */
          }
          if (cur.part >= cur.nparts) {
               finishcur();
               continue;
          }
          if (!step()) {
               return;             /* GME is busy: next timer run         */
          }
     }
}

/*
 * Start on the next downloaded article: find its forum, skip it if the
 * forum already has it, find the message it replies to, and build the
 * BBS text.
 */
static BOOL                        /*   FALSE when nothing is waiting      */
loadnext(VOID)
{
     CHAR path[MAX_PATH],sid[BNSP_IDSZ];
     struct bnmid rec;
     CHAR *body=NULL;
     LONG bodylen=0;
     INT i;

     if (npend == 0) {
          return(FALSE);
     }
     stzcpy(sid,pend[0],sizeof(sid));
     if (npend > 1) {
          memmove(&pend[0],&pend[1],(npend-1)*sizeof(pend[0]));
     }
     npend--;

     setmem((CHAR *)&cur,sizeof(cur),0);
     bnspoolPath(path,indir,sid,"RDY");
     if (!bnspoolRead(path,&cur.r)) {
          bnspoolDelete(indir,sid);     /* gone or damaged: forget it      */
          setmem((CHAR *)&cur,sizeof(cur),0);
          return(TRUE);
     }

     /* the link it came through must still exist and still import */
     if ((cur.gidx=grpfind(cur.r.group)) < 0 || grps[cur.gidx].doimp != 'Y'
      || (cur.fid=getfid(grps[cur.gidx].forum)) == EMLID) {
          if (cur.gidx >= 0 && grps[cur.gidx].doimp == 'Y'
           && cfg.auditbad == 'Y') {
               audit("NEWSGROUP FORUM MISSING","%.30s: no forum %.16s",
                     cur.r.group,grps[cur.gidx].forum);
          }
          bnspoolDelete(indir,sid);
          setmem((CHAR *)&cur,sizeof(cur),0);
          return(TRUE);
     }

     /*
      * Already in this forum?  That is an article this BBS posted itself
      * coming back from the news server, or one read again after the
      * server renumbered the group, or one cross-posted to two newsgroups
      * linked to the same forum.
      */
     if (midfind(cur.r.msgid,cur.fid,&rec)) {
          bnspoolDelete(indir,sid);
          setmem((CHAR *)&cur,sizeof(cur),0);
          return(TRUE);
     }

     /* the forum message it replies to: nearest known ancestor */
     for (i=0 ; i < cur.r.npar ; i++) {
          if (midfind(cur.r.parents[i],cur.fid,&rec)) {
               cur.parent.sysid=rec.gsys;
               cur.parent.msgid=rec.gmsg;
               break;
          }
     }
     articledate(cur.r.date,&cur.crdate,&cur.crtime);

     if (!cur.r.oversize) {
          bnspoolPath(path,indir,sid,"TXT");
          body=sbReadFile(path,&bodylen,8L*1024L*1024L);
     }
     buildtext(body,(size_t)bodylen);
     free(body);
     splitparts();
     cur.active=TRUE;
     return(TRUE);
}

/*
 * One impmsg() call for the current part.  Returns FALSE when GME asks
 * to be called again later.
 */
static BOOL
step(VOID)
{
     INT rc;

     if (!cur.again) {
          buildmsg();
          inigmerq(workarea);
     }
     /* no attached file: an empty name, as GALSMTP's importer passes it */
     rc=impmsg(workarea,&cur.msg,partbuf,"");
     if (rc == GMEAGAIN) {
          cur.again=TRUE;
          return(FALSE);
     }
     cur.again=FALSE;
     if (gmerqopn(workarea)) {
          clsgmerq(workarea);
     }
     switch (rc) {
     case GMEOK:
     case GMEAFWD:
     case GMERRG:
          if (cur.part == 0) {
               cur.first=cur.msg.gmid;
               cur.imported=TRUE;
               midadd(cur.r.msgid,cur.fid,&cur.msg.gmid,cur.r.root);
          }
          break;
     default:
          if (cfg.auditbad == 'Y') {
               audit("NEWSGROUP IMPORT ERROR","%.30s #%I64d: GME code %d",
                     cur.r.group,cur.r.artnum,rc);
          }
          cur.part=cur.nparts;     /* skip the rest of this article       */
          return(TRUE);
     }
     cur.part++;
     return(TRUE);
}

/*
 * The article is in (or could not be put in): remove its spool files.
 * The Message-ID record written when part 1 went in is what keeps it
 * from being imported twice, so a restart between the two is harmless.
 */
static VOID
finishcur(VOID)
{
     if (cur.imported) {
          INT gi=grpfind(cur.r.group);   /* (the list may have changed)   */

          nimported++;
          if (gi >= 0) {
               grps[gi].nimp++;
               grpsave(gi,NULL);
          }
     }
     bnspoolDelete(indir,cur.r.sid);
     dropcur();
}

static VOID
dropcur(VOID)                      /* release the current article          */
{
     free(cur.text);
     setmem((CHAR *)&cur,sizeof(cur),0);
}

/*
 * The whole text in BBS form (lines end in a bare CR): the sender lines
 * (if switched on), the article text (or the too-large notice), and a
 * note about attachments that were left out.
 */
static VOID
buildtext(
const CHAR *body,
size_t bodylen)
{
     size_t i;

     if (cur.r.fromname[0] != '\0') {
          _snprintf(tvfrom,sizeof(tvfrom)-1,"%s <%s>",cur.r.fromname,
                    cur.r.from);
          tvfrom[sizeof(tvfrom)-1]='\0';
     }
     else {
          stzcpy(tvfrom,cur.r.from,sizeof(tvfrom));
     }
     stzcpy(tvdate,cur.r.date,sizeof(tvdate));
     if (cfg.showhdr == 'Y') {
          addtext(msgtext(SNDLINE));
     }
     if (cur.r.oversize) {
          sprintf(tvsize,"%ld",(cur.r.size+1023L)/1024L);
          sprintf(tvlimit,"%d",cfg.maxkb);
          addtext(msgtext(OVERSZ));
     }
     else if (body != NULL && bodylen > 0) {
          addtext(body);
     }
     if (cur.r.natt > 0) {
          sprintf(tvnatt,"%d",cur.r.natt);
          addtext(msgtext(ATTNOTE));
     }
     if (cur.text == NULL) {
          addtext("");
     }
     /* BBS text: lines end in a bare CR */
     for (i=0 ; i < cur.textlen ; i++) {
          if (cur.text[i] == '\n') {
               cur.text[i]='\r';
          }
     }
}

static VOID
addtext(                           /* append to the text being built       */
const CHAR *s)
{
     size_t n=strlen(s);
     CHAR *bigger;

     if ((bigger=(CHAR *)realloc(cur.text,cur.textlen+n+1)) == NULL) {
          return;
     }
     cur.text=bigger;
     memcpy(&cur.text[cur.textlen],s,n+1);
     cur.textlen+=n;
}

/*
 * Cut the text into parts that fit one BBS message, at line ends where
 * possible.  Room is left for the "(part N of M)" line.
 */
static VOID
splitparts(VOID)
{
     size_t room=(size_t)TXTLEN-80,pos=0;

     cur.nparts=0;
     cur.offs[0]=0;
     while (cur.nparts < MAXPARTS) {
          size_t end;

          if (cur.textlen-pos <= room) {
               cur.offs[++cur.nparts]=cur.textlen;
               return;
          }
          end=pos+room;
          while (end > pos+room/2 && cur.text[end-1] != '\r') {
               end--;
          }
          if (end <= pos+room/2) {
               end=pos+room;       /* no line end nearby: cut anyway      */
          }
          cur.offs[++cur.nparts]=end;
          pos=end;
     }
     /* longer than MAXPARTS messages: the rest is not imported */
     cur.textlen=cur.offs[cur.nparts];
}

/*
 * The message header and text for the current part.  Part 1 replies to
 * the article's parent (GME then files it in the parent's thread); later
 * parts reply to part 1.
 */
static VOID
buildmsg(VOID)
{
     struct addrinf ai;
     const CHAR *adr;
     size_t len=0,n;

     setmem((CHAR *)&cur.msg,sizeof(cur.msg),0);
     cur.msg.forum=cur.fid;
     /*
      * The sender in GALSMTP's form (IN:name@host), so "reply by e-mail"
      * from the forum reaches the author through the BBS's Internet mail.
      * Replies go to Reply-To when there is one, as with GALSMTP.
      */
     adr=(cur.r.replyto[0] != '\0') ? cur.r.replyto : cur.r.from;
     parseIntAddr(adr,&ai);
     smtpCvtFrom(cur.msg.from,&ai);
     stzcpy(cur.msg.to,ALLINF,MAXADR);
     stzcpy(cur.msg.topic,cur.r.subject[0] != '\0' ? cur.r.subject : "<none>",
            TPCSIZ);
     sanitize(cur.msg.topic);
     cur.msg.crdate=cur.crdate;
     cur.msg.crtime=cur.crtime;
     if (cur.part == 0) {
          cur.msg.rplto=cur.parent;
     }
     else {
          cur.msg.rplto=cur.first;
     }

     partbuf[0]='\0';
     if (cur.nparts > 1) {
          sprintf(tvpart,"%d",cur.part+1);
          sprintf(tvnparts,"%d",cur.nparts);
          stzcpy(partbuf,msgtext(PARTLINE),TXTLEN);
          for (len=0 ; partbuf[len] != '\0' ; len++) {
               if (partbuf[len] == '\n') {
                    partbuf[len]='\r';
               }
          }
     }
     n=cur.offs[cur.part+1]-cur.offs[cur.part];
     if (len+n >= (size_t)TXTLEN) {
          n=(size_t)TXTLEN-len-1;
     }
     memcpy(&partbuf[len],&cur.text[cur.offs[cur.part]],n);
     partbuf[len+n]='\0';
}

/*
 * The article's own date, in GME's packed form (as DOS stores dates),
 * in the BBS's local time.  A date that cannot be read, or one in the
 * future, gives 0: GME then uses "now".
 */
static VOID
articledate(
const CHAR *date,
USHORT *d,
USHORT *t)
{
     time_t utc,now=time(NULL);
     struct tm lt;

     *d=*t=0;
     if (!bnartParseDate(date,&utc) || utc > now+3600
      || localtime_s(&lt,&utc) != 0 || lt.tm_year < 80) {
          return;
     }
     *d=(USHORT)(((lt.tm_year-80)<<9)|((lt.tm_mon+1)<<5)|lt.tm_mday);
     *t=(USHORT)((lt.tm_hour<<11)|(lt.tm_min<<5)|(lt.tm_sec/2));
}

/*
 * A message from SNBETNWS.MSG with its text variables filled in, ANSI
 * removed, as plain text.  getmsg() is used rather than prfmsg() so the
 * output buffer of whichever user is online is never touched.
 */
static const CHAR *
msgtext(
INT msgno)
{
     static CHAR buf[2048];

     setmbk(bnmb);
     stzcpy(buf,getmsg(msgno),sizeof(buf));
     rstmbk();
     stpans(buf);
     return(buf);
}

static VOID
loadspool(VOID)                    /* downloaded articles at startup       */
{
     WIN32_FIND_DATAA fd;
     HANDLE h;
     CHAR pattern[MAX_PATH];

     _snprintf(pattern,sizeof(pattern)-1,"%s*.RDY",indir);
     pattern[sizeof(pattern)-1]='\0';
     if ((h=FindFirstFileA(pattern,&fd)) == INVALID_HANDLE_VALUE) {
          return;
     }
     do {
          CHAR *dot=strchr(fd.cFileName,'.');

          if (dot != NULL && dot-fd.cFileName < BNSP_IDSZ) {
               *dot='\0';
               addpend(fd.cFileName);
          }
     } while (FindNextFileA(h,&fd));
     FindClose(h);
     /* spool ids grow with time: sorted, they are in arrival order */
     if (npend > 1) {
          qsort(pend,npend,sizeof(pend[0]),pendcmp);
     }
}

static VOID
addpend(                           /* add to the end of the import line    */
const CHAR *sid)
{
     if (npend >= pendroom) {
          CHAR (*bigger)[BNSP_IDSZ];

          bigger=(CHAR (*)[BNSP_IDSZ])realloc(pend,(pendroom+256)
                                              *sizeof(pend[0]));
          if (bigger == NULL) {
               return;             /* picked up again at the next restart */
          }
          pend=bigger;
          pendroom+=256;
     }
     stzcpy(pend[npend++],sid,BNSP_IDSZ);
}

static INT
pendcmp(
const VOID *a,
const VOID *b)
{
     return(strcmp((const CHAR *)a,(const CHAR *)b));
}

/* ======================================================================== */
/*   Posting forum messages to Usenet                                        */
/* ======================================================================== */

/*
 * Called by GME for every new message.  A message a user wrote (or
 * replied with) in a forum linked for export goes into the outgoing
 * queue.  Imported messages (NEWMSG_IMPRT: ours, and other networks')
 * are never posted, so nothing loops between networks.
 *
 * Runs on the main thread inside the user's "post" action, so it only
 * does quick file work.  The posting happens later, on the worker thread.
 */
static VOID
bnnewmsg(
INT newtype,
const struct message *msg,
const CHAR *text)
{
     CHAR groups[256];
     INT i;

     if (!running || cfg.enabled != 'Y' || msg->forum == EMLID
      || (newtype != NEWMSG_WRITE && newtype != NEWMSG_REPLY)) {
          return;
     }
     groups[0]='\0';
     for (i=0 ; i < ngrps ; i++) {
          if (grps[i].doexp != 'Y' || getfid(grps[i].forum) != msg->forum) {
               continue;
          }
          if (strlen(groups)+strlen(grps[i].name)+2 >= sizeof(groups)) {
               break;
          }
          if (groups[0] != '\0') {
               strcat(groups,",");
          }
          strcat(groups,grps[i].name);
     }
     if (groups[0] == '\0') {
          return;                  /* forum not linked for export         */
     }
     if (cfg.postkey[0] != '\0' && uhskey((CHAR *)msg->from,cfg.postkey) != 1) {
          return;                  /* author may not post to Usenet       */
     }
     queuepost(msg,text,groups);
}

/*
 * Turn the forum message into a Usenet article and queue it.  The
 * article is built now, while everything about the message is at hand;
 * the worker only has to send the file.  The Message-ID is recorded at
 * once, so replies to this message carry a References header and the
 * copy the news server sends back is recognized.
 */
static VOID
queuepost(
const struct message *msg,
const CHAR *text,
const CHAR *groups)
{
     BNPOST p;
     OQITEM q;
     struct bnmid parent;
     CHAR sig[1024],path[MAX_PATH],tmpl[128];
     CHAR *art;
     size_t len;
     FILE *fp;
     GBOOL isrep;

     setmem((CHAR *)&p,sizeof(p),0);
     stzcpy(p.newsgroups,groups,sizeof(p.newsgroups));
     fillname(tmpl,sizeof(tmpl),cfg.fromname,msg->from);
     stzcpy(p.fromname,tmpl,sizeof(p.fromname));
     cvtfrom(p.fromadr,msg->from);
     isrep=(msg->rplto.sysid != 0L && msg->rplto.msgid != 0L);
     if (isrep && !sameto("RE:",msg->topic)) {
          _snprintf(p.subject,sizeof(p.subject)-1,"Re: %s",msg->topic);
     }
     else {
          stzcpy(p.subject,msg->topic,sizeof(p.subject));
     }
     sanitize(p.subject);
     sbRfc5322Date(p.date,sizeof(p.date));
     bnartMakeMsgId(p.msgid,sizeof(p.msgid),(ULONG)msg->gmid.sysid,
                    (ULONG)msg->gmid.msgid,ourhost());
     if (isrep && midbygid(&msg->rplto,&parent)) {
          stzcpy(p.parent,parent.mid,sizeof(p.parent));
          stzcpy(p.root,parent.root[0] != '\0' ? parent.root : parent.mid,
                 sizeof(p.root));
     }
     stzcpy(p.org,cfg.org,sizeof(p.org));
     _snprintf(p.useragent,sizeof(p.useragent)-1,"%s/%s (The Major BBS v10)",
               BNPRODUCT,BNVER_DOTTED);
     /* "Better Newsgroups/1.0.0.1": no spaces in the product token */
     {
          CHAR *s=strchr(p.useragent,' ');
          if (s != NULL && s < strchr(p.useragent,'/')) {
               *s='-';
          }
     }
     stzcpy(sig,msgtext(TAGLINE),sizeof(sig));
     if (strspn(sig," \t\r\n") == strlen(sig)) {
          sig[0]='\0';             /* a blank TAGLINE means no signature  */
     }

     if ((art=bnartBuild(&p,text,sig,&len)) == NULL) {
          audit("NEWSGROUP POST NOT QUEUED","Out of memory: message by %.29s",
                msg->from);
          return;
     }
     setmem((CHAR *)&q,sizeof(q),0);
     q.qid=newqid();
     q.queued=q.nexttry=time(NULL);
     stzcpy(q.author,msg->from,sizeof(q.author));
     stzcpy(q.groups,groups,sizeof(q.groups));
     stzcpy(q.topic,msg->topic,sizeof(q.topic));
     sanitize(q.topic);
     stzcpy(q.msgid,p.msgid,sizeof(q.msgid));

     oqpath(path,q.qid,"ART");
     if ((fp=fopen(path,"wb")) == NULL || fwrite(art,1,len,fp) != len) {
          if (fp != NULL) {
               fclose(fp);
          }
          DeleteFileA(path);
          free(art);
          audit("NEWSGROUP POST NOT QUEUED","Cannot write to SNBETNWS\\OUT");
          return;
     }
     fclose(fp);
     free(art);
     if (noutq >= outroom) {
          OQITEM *bigger=(OQITEM *)realloc(outq,(outroom+32)*sizeof(OQITEM));
          if (bigger == NULL) {
               DeleteFileA(path);
               return;
          }
          outq=bigger;
          outroom+=32;
     }
     if (!writeque(&q)) {
          DeleteFileA(path);
          audit("NEWSGROUP POST NOT QUEUED","Cannot write to SNBETNWS\\OUT");
          return;
     }
     memcpy(&outq[noutq++],&q,sizeof(q));
     midadd(p.msgid,msg->forum,&msg->gmid,p.root);
}

/*
 * Tell the author, in BBS e-mail, that a post did not make it to Usenet
 * (the forum still has it).
 */
static VOID
rejectnotice(
const OQITEM *q,
const CHAR *why)
{
     struct message msg;
     CHAR *p;

     if (q->author[0] == '\0' || partbuf == NULL) {
          return;
     }
     stzcpy(tvrjgrp,q->groups,sizeof(tvrjgrp));
     stzcpy(tvrjsubj,q->topic,sizeof(tvrjsubj));
     stzcpy(tvrjwhy,why,sizeof(tvrjwhy));
     setmem((CHAR *)&msg,sizeof(msg),0);
     stlcpy(msg.from,smtfpx,MAXADR);
     stlcat(msg.from,MAILDAEM "@",MAXADR);
     stlcat(msg.from,smtpHost(),MAXADR);
     stzcpy(msg.to,q->author,MAXADR);
     stzcpy(msg.topic,"Newsgroup post not sent",TPCSIZ);
     stzcpy(partbuf,msgtext(REJECT),TXTLEN);
     for (p=partbuf ; *p != '\0' ; p++) {
          if (*p == '\n') {
               *p='\r';            /* BBS message text: bare CR lines     */
          }
     }
     simpsnd(&msg,partbuf,NULL);
}

static VOID
loadoutq(VOID)                     /* outgoing queue at startup            */
{
     WIN32_FIND_DATAA fd;
     HANDLE h;
     CHAR pattern[MAX_PATH],path[MAX_PATH];
     OQITEM q;

     _snprintf(pattern,sizeof(pattern)-1,"%s*.QUE",outdir);
     pattern[sizeof(pattern)-1]='\0';
     if ((h=FindFirstFileA(pattern,&fd)) == INVALID_HANDLE_VALUE) {
          return;
     }
     do {
          _snprintf(path,sizeof(path)-1,"%s%s",outdir,fd.cFileName);
          path[sizeof(path)-1]='\0';
          if (!readque(path,&q)) {
               continue;
          }
          if (noutq >= outroom) {
               OQITEM *bigger=(OQITEM *)realloc(outq,(outroom+32)
                                                *sizeof(OQITEM));
               if (bigger == NULL) {
                    break;
               }
               outq=bigger;
               outroom+=32;
          }
          memcpy(&outq[noutq++],&q,sizeof(q));
          if (q.qid > lastqid) {
               lastqid=q.qid;
          }
     } while (FindNextFileA(h,&fd));
     FindClose(h);
}

/*
 * The .QUE file is plain "name=value" text, so a Sysop can look at a
 * stuck post with Notepad.  Written to a temporary file first and then
 * swapped in, so a crash mid-write never leaves a half-written entry.
 */
static GBOOL
writeque(
const OQITEM *q)
{
     CHAR path[MAX_PATH],tmp[MAX_PATH];
     FILE *fp;

     oqpath(path,q->qid,"QUE");
     oqpath(tmp,q->qid,"QTM");
     if ((fp=fopen(tmp,"w")) == NULL) {
          return(FALSE);
     }
     fprintf(fp,"version=1\n");
     fprintf(fp,"queued=%lld\n",(long long)q->queued);
     fprintf(fp,"nexttry=%lld\n",(long long)q->nexttry);
     fprintf(fp,"tries=%d\n",q->tries);
     fprintf(fp,"author=%s\n",q->author);
     fprintf(fp,"groups=%s\n",q->groups);
     fprintf(fp,"topic=%s\n",q->topic);
     fprintf(fp,"msgid=%s\n",q->msgid);
     fprintf(fp,"lasterr=%s\n",q->lasterr);
     if (fclose(fp) != 0) {
          DeleteFileA(tmp);
          return(FALSE);
     }
     if (!MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING)) {
          DeleteFileA(tmp);
          return(FALSE);
     }
     return(TRUE);
}

static GBOOL
readque(                           /* load one .QUE file                   */
const CHAR *path,
OQITEM *q)
{
     FILE *fp;
     CHAR line[512],*eq,*val,*fname;
     CHAR art[MAX_PATH];

     setmem((CHAR *)q,sizeof(*q),0);
     fname=strrchr(path,'\\');
     q->qid=strtol(fname != NULL ? fname+1 : path,NULL,16);
     if (q->qid <= 0 || (fp=fopen(path,"r")) == NULL) {
          return(FALSE);
     }
     while (fgets(line,sizeof(line),fp) != NULL) {
          line[strcspn(line,"\r\n")]='\0';
          if ((eq=strchr(line,'=')) == NULL) {
               continue;
          }
          *eq='\0';
          val=eq+1;
          if (sameas(line,"queued")) {
               q->queued=(time_t)_atoi64(val);
          }
          else if (sameas(line,"nexttry")) {
               q->nexttry=(time_t)_atoi64(val);
          }
          else if (sameas(line,"tries")) {
               q->tries=atoi(val);
          }
          else if (sameas(line,"author")) {
               stzcpy(q->author,val,sizeof(q->author));
          }
          else if (sameas(line,"groups")) {
               stzcpy(q->groups,val,sizeof(q->groups));
          }
          else if (sameas(line,"topic")) {
               stzcpy(q->topic,val,sizeof(q->topic));
          }
          else if (sameas(line,"msgid")) {
               stzcpy(q->msgid,val,sizeof(q->msgid));
          }
          else if (sameas(line,"lasterr")) {
               stzcpy(q->lasterr,val,sizeof(q->lasterr));
          }
     }
     fclose(fp);
     oqpath(art,q->qid,"ART");
     return(q->groups[0] != '\0'
         && GetFileAttributesA(art) != INVALID_FILE_ATTRIBUTES);
}

static VOID
oqpath(                            /* full path of an outgoing queue file  */
CHAR *dst,
LONG qid,
const CHAR *ext)
{
     _snprintf(dst,MAX_PATH-1,"%s%08lX.%s",outdir,(unsigned long)qid,ext);
     dst[MAX_PATH-1]='\0';
}

static VOID
oqdelete(                          /* remove a post's files and entry      */
INT idx)
{
     CHAR path[MAX_PATH];

     oqpath(path,outq[idx].qid,"QUE");
     DeleteFileA(path);
     oqpath(path,outq[idx].qid,"ART");
     DeleteFileA(path);
     if (idx < noutq-1) {
          memmove(&outq[idx],&outq[idx+1],(noutq-idx-1)*sizeof(OQITEM));
     }
     noutq--;
}

static INT
oqfind(                            /* index of a queued post, -1 if gone   */
LONG qid)
{
     INT i;

     for (i=0 ; i < noutq ; i++) {
          if (outq[i].qid == qid) {
               return(i);
          }
     }
     return(-1);
}

static LONG
newqid(VOID)                       /* a fresh queue id (file name)         */
{
     CHAR path[MAX_PATH];
     LONG id=(LONG)time(NULL)&0x7FFFFFFFL;

     /* ids increase over time; skip any that are somehow already in use */
     if (id <= lastqid) {
          id=lastqid+1;
     }
     for (;;) {
          oqpath(path,id,"QUE");
          if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES
           && oqfind(id) < 0) {
               break;
          }
          id++;
     }
     lastqid=id;
     return(id);
}

/*
 * A BBS user's Internet address, worked out exactly the way GALSMTP's own
 * sender does it: GALSMTP's address translation first, otherwise the
 * user's alias (or converted User-ID) at the BBS's mail host name.
 */
static VOID
cvtfrom(
CHAR *dst,                         /*   output, MAXADR bytes               */
const CHAR *src)                   /*   BBS sender (User-ID)               */
{
     stlcpy(dst,src,MAXADR);
     if (!xltGME2Int(dst)) {
          stlcpy(dst,(*alsofusr)((CHAR *)src),MAXADR);
          stlcat(dst,"@",MAXADR);
          stlcat(dst,smtpHost(),MAXADR);
     }
}

static VOID
fillname(                          /* expand {USER} and {BBS} in From name */
CHAR *dst,
size_t dstsz,
const CHAR *tmpl,
const CHAR *userid)
{
     const CHAR *p;
     size_t len=0;

     for (p=tmpl ; *p != '\0' && len < dstsz-1 ; ) {
          const CHAR *sub=NULL;
          size_t skip=0;

          if (_strnicmp(p,"{USER}",6) == 0) {
               sub=userid;
               skip=6;
          }
          else if (_strnicmp(p,"{BBS}",5) == 0) {
               sub=bbsttl;
               skip=5;
          }
          if (sub != NULL) {
               while (*sub != '\0' && len < dstsz-1) {
                    dst[len++]=*sub++;
               }
               p+=skip;
          }
          else {
               dst[len++]=*p++;
          }
     }
     dst[len]='\0';
     sanitize(dst);
}

static const CHAR *
ourhost(VOID)                      /* host name for our Message-IDs        */
{
     return(cfg.domain[0] != '\0' ? cfg.domain : smtpHost());
}

/* ======================================================================== */
/*   Message-ID cross-reference (SNBETMID.DAT)                               */
/* ======================================================================== */

static GBOOL                       /*   TRUE if this forum has the article */
midfind(
const CHAR *mid,
USHORT forum,
struct bnmid *out)
{
     struct bnmid key;
     GBOOL found;

     if (midbb == NULL || mid[0] == '\0') {
          return(FALSE);
     }
     /*
      * The key (forum, then the start of the Message-ID: the record's
      * first bytes, in key order) is built in writable memory,
      * zero-padded, exactly as it is stored.
      */
     setmem((CHAR *)&key,sizeof(key),0);
     stzcpy(key.mid,mid,sizeof(key.mid));
     key.forum=forum;
     dfaSetBlk(midbb);
     found=dfaAcqEQ(out,&key,0);
     dfaRstBlk();
     return(found);
}

static GBOOL                       /*   TRUE if the message is an article  */
midbygid(
const struct globid *g,
struct bnmid *out)
{
     struct gidkey key;
     GBOOL found;

     if (midbb == NULL) {
          return(FALSE);
     }
     key.gsys=g->sysid;
     key.gmsg=g->msgid;
     dfaSetBlk(midbb);
     found=dfaAcqEQ(out,&key,1);
     dfaRstBlk();
     return(found);
}

static VOID
midadd(                            /* record a forum message's Message-ID  */
const CHAR *mid,
USHORT forum,
const struct globid *g,
const CHAR *root)
{
     struct bnmid rec;

     if (midbb == NULL || mid[0] == '\0') {
          return;
     }
     setmem((CHAR *)&rec,sizeof(rec),0);
     stzcpy(rec.mid,mid,sizeof(rec.mid));
     rec.forum=forum;
     rec.gsys=g->sysid;
     rec.gmsg=g->msgid;
     rec.added=(LONG)time(NULL);
     stzcpy(rec.root,root != NULL ? root : "",sizeof(rec.root));
     dfaSetBlk(midbb);
     dfaInsertDup(&rec);           /* a duplicate is already recorded     */
     dfaRstBlk();
}

/*
 * Forget records older than the "Keep Message-IDs" setting, a few per
 * timer run, oldest first (key 2).  Once nothing old is left, look again
 * in an hour.
 */
static VOID
midpurge(VOID)
{
     struct bnmid rec;
     LONG cutoff=(LONG)(time(NULL)-(time_t)cfg.keepdays*86400L);
     INT n;

     if (midbb == NULL) {
          nextpurge=time(NULL)+3600;
          return;
     }
     dfaSetBlk(midbb);
     for (n=0 ; n < PURGEPERTICK ; n++) {
          if (!dfaAcqLO(&rec,2) || rec.added >= cutoff) {
               nextpurge=time(NULL)+3600;
               break;
          }
          dfaDelete();             /* directly after the acquire          */
     }
     dfaRstBlk();
}

/* ======================================================================== */
/*   Settings and newsgroup records (SNBETNWS.DAT)                           */
/* ======================================================================== */

static GBOOL
configured(VOID)                   /* enough settings to reach the server? */
{
     return(missing() == NULL);
}

static const CHAR *                /*   the first setting missing, or NULL */
missing(VOID)
{
     if (cfg.host[0] == '\0') {
          return("News server");
     }
     if (cfg.port <= 0) {
          return("News server port");
     }
     return(NULL);
}

static VOID
cfgdefaults(VOID)                  /* settings for a brand-new install     */
{
     setmem((CHAR *)&cfg,sizeof(cfg),0);
     stzcpy(cfg.recid,CFGKEY,sizeof(cfg.recid));
     cfg.version=CFGVER;
     cfg.enabled='N';              /* off until the Sysop sets it up      */
     cfg.security=BNSEC_NONE;
     cfg.verify='Y';
     cfg.auditok='N';
     cfg.auditbad='Y';
     cfg.translog='N';
     cfg.showhdr='Y';
     cfg.port=119;
     cfg.pollmin=10;
     cfg.timeout=60;
     cfg.backfill=50;
     cfg.maxper=200;
     cfg.maxkb=64;
     cfg.wrapcol=79;
     cfg.retrymin=15;
     cfg.giveuphrs=48;
     cfg.keepdays=365;
     stzcpy(cfg.fromname,"{USER}",sizeof(cfg.fromname));
}

static VOID
cfgload(VOID)                      /* read the settings record             */
{
     struct bncfg rec;

     cfgdefaults();
     if (cfgbb == NULL) {
          audit("NEWSGROUPS SETTINGS","SNBETNWS.DAT could not be opened; "
                "using defaults");
          return;
     }
     /* the lookup key must be in writable memory, never a literal */
     setmem((CHAR *)&rec,sizeof(rec),0);
     stzcpy(rec.recid,CFGKEY,sizeof(rec.recid));
     dfaSetBlk(cfgbb);
     if (dfaAcqEQ(&rec,rec.recid,0)) {
          if (rec.version == CFGVER) {
               memcpy(&cfg,&rec,sizeof(cfg));
          }
          dfaRstBlk();
          return;
     }
     dfaRstBlk();
     cfgsave();                    /* first run: store the defaults       */
}

static GBOOL
cfgsave(VOID)                      /* write the settings record            */
{
     struct bncfg rec;
     GBOOL ok;

     if (cfgbb == NULL) {
          return(FALSE);
     }
     setmem((CHAR *)&rec,sizeof(rec),0);
     stzcpy(rec.recid,CFGKEY,sizeof(rec.recid));
     dfaSetBlk(cfgbb);
     /* an update must directly follow an acquire of the same record */
     if (dfaAcqEQ(&rec,rec.recid,0)) {
          ok=dfaUpdateDup(&cfg);
     }
     else {
          ok=dfaInsertDup(&cfg);
     }
     dfaRstBlk();
     if (!ok) {
          audit("NEWSGROUPS SETTINGS","Could not save SNBETNWS.DAT");
     }
     return(ok);
}

/*
 * Every newsgroup record, at startup.  A read-only walk in file order
 * (dfaStep does not need the key order and takes no locks).
 */
static VOID
grpload(VOID)
{
     struct bngrp rec;
     GBOOL more;

     ngrps=0;
     if (cfgbb == NULL) {
          return;
     }
     dfaSetBlk(cfgbb);
     for (more=dfaStepLO(&rec) ; more && ngrps < MAXGRPS ;
          more=dfaStepNX(&rec)) {
          if (rec.recid[0] == '!' || rec.kind != 'G' || rec.version != GRPVER) {
               continue;
          }
          memcpy(&grps[ngrps++],&rec,sizeof(rec));
     }
     dfaRstBlk();
}

/*
 * Write a newsgroup record.  When the newsgroup was renamed, its key
 * changed: the record under the old key is removed first.
 */
static GBOOL
grpsave(
INT i,
const CHAR *oldkey)                /*   key before a rename, or NULL       */
{
     struct bngrp rec;
     CHAR key[RECKEYSZ];
     GBOOL ok;

     if (cfgbb == NULL) {
          return(FALSE);
     }
     grpkey(grps[i].recid,grps[i].name);
     grps[i].version=GRPVER;
     grps[i].kind='G';
     dfaSetBlk(cfgbb);
     if (oldkey != NULL && strcmp(oldkey,grps[i].recid) != 0) {
          setmem(key,sizeof(key),0);
          stzcpy(key,oldkey,sizeof(key));
          if (dfaAcqEQ(&rec,key,0)) {
               dfaDelete();
          }
     }
     setmem(key,sizeof(key),0);
     stzcpy(key,grps[i].recid,sizeof(key));
     if (dfaAcqEQ(&rec,key,0)) {
          ok=dfaUpdateDup(&grps[i]);
     }
     else {
          ok=dfaInsertDup(&grps[i]);
     }
     dfaRstBlk();
     if (!ok) {
          audit("NEWSGROUPS SETTINGS","Could not save %.40s",grps[i].name);
     }
     return(ok);
}

static GBOOL
grpremove(                         /* delete a newsgroup link              */
INT i)
{
     struct bngrp rec;
     CHAR key[RECKEYSZ];
     GBOOL ok=TRUE;

     if (cfgbb != NULL) {
          setmem(key,sizeof(key),0);
          stzcpy(key,grps[i].recid,sizeof(key));
          dfaSetBlk(cfgbb);
          if (dfaAcqEQ(&rec,key,0)) {
               dfaDelete();
          }
          dfaRstBlk();
     }
     if (i < ngrps-1) {
          memmove(&grps[i],&grps[i+1],(ngrps-i-1)*sizeof(grps[0]));
     }
     ngrps--;
     return(ok);
}

static INT                         /*   index, or -1                       */
grpfind(
const CHAR *name)
{
     INT i;

     for (i=0 ; i < ngrps ; i++) {
          if (sameas(grps[i].name,name)) {
               return(i);
          }
     }
     return(-1);
}

static VOID
grpkey(                            /* record key for a newsgroup name      */
CHAR *dst,                         /*   RECKEYSZ bytes, zero-padded        */
const CHAR *name)
{
     setmem(dst,RECKEYSZ,0);
     stzcpy(dst,name,RECKEYSZ);
     strlwr(dst);                  /* newsgroup names are not case-        */
}                                  /* sensitive here                       */

/* ======================================================================== */
/*   Sysop menu                                                              */
/* ======================================================================== */

GBOOL
bnentry(VOID)                      /* input while the user is in the module*/
{
     CHAR line[256];
     INT n,i;

     setmbk(bnmb);
     if (haskey("SYSOP") != 1) {
          prfmsg(SYSONLY);
          outprf(usrnum);
          rstmbk();
          return(0);
     }
     btuxnf(usrnum,0,19);          /* no "More?" pauses on these screens  */
     /*
      * The whole line as typed (rstrin() puts back the spaces the BBS took
      * out when it split the line into words), trimmed at both ends, so
      * values such as an Organization can contain spaces.
      */
     rstrin();
     {
          CHAR *b=input;

          while (*b == ' ') {
               b++;
          }
          stzcpy(line,b,sizeof(line));
          n=(INT)strlen(line);
          while (n > 0 && line[n-1] == ' ') {
               line[--n]='\0';
          }
     }

     menushown=FALSE;
     switch (usrptr->substt) {
     case 0:                       /* just arrived                        */
          usrptr->substt=ST_MENU;
          curpage[usrnum]=0;
          showmenu();
          break;
     case ST_MENU:
          if (line[0] == '\0' || sameas(line,"?")) {
               showmenu();
          }
          else if (sameas(line,"X")) {
               prfmsg(LEAVE);
               outprf(usrnum);
               rstrxf();
               rstmbk();
               return(0);
          }
          else if (sameas(line,"N") || sameas(line,"P")) {
               curpage[usrnum]=(curpage[usrnum]+1)%NPAGES;
               showmenu();
          }
          else if (sameas(line,"G")) {
               usrptr->substt=ST_GROUPS;
               grptop[usrnum]=0;
               showgroups();
          }
          else if (sameas(line,"C")) {
               if (!configured()) {
                    stzcpy(tvhint,"the News server setting is blank",
                           sizeof(tvhint));
                    prfmsg(CHECKBAD);
               }
               else if (busy) {
                    stzcpy(tvhint,"a session is running right now",
                           sizeof(tvhint));
                    prfmsg(CHECKBAD);
               }
               else {
                    checknow=TRUE;
                    setstatus();
                    prfmsg(CHECKOK);
               }
          }
          else if (sameas(line,"Q")) {
               showqueue();
          }
          else if (sameas(line,"H")) {
               prfmsg(HELPMENU);
          }
          else if ((line[0] == 'H' || line[0] == 'h' || line[0] == '?')
                && (n=atoi(line+1+strspn(line+1," "))) >= 1 && n <= NFIELDS) {
               const FIELD *f=&fields[n-1];     /* "H 12": help on a setting */

               sprintf(tvrownum,"%d",n);
               stzcpy(tvrowname,f->name,sizeof(tvrowname));
               stzcpy(tvhint,f->hint,sizeof(tvhint));
               prfmsg(f->help);
          }
          else if ((n=atoi(line)) >= 1 && n <= NFIELDS
                && strspn(line,"0123456789") == strlen(line)) {
               editfld[usrnum]=n-1;
               startedit(&fields[n-1],n,ST_VALUE,ST_CHOICE);
          }
          else {
               prfmsg(BADSEL);
          }
          break;
     case ST_VALUE:
     case ST_GVALUE:
          {
               GBOOL ingrp=(usrptr->substt == ST_GVALUE);
               const FIELD *f=ingrp ? &gfields[editfld[usrnum]]
                                    : &fields[editfld[usrnum]];

               if (sameas(line,"?")) {
                    prfmsg(f->help);
                    prfmsg(ASKVAL);
                    break;
               }
               usrptr->substt=ingrp ? ST_GEDIT : ST_MENU;
               if (line[0] == '\0') {
                    if (ingrp) {
                         showgroup();
                    }
                    else {
                         showmenu();
                    }
                    break;
               }
               applyvalue(f,line);
          }
          break;
     case ST_CHOICE:               /* a list setting: number or name      */
     case ST_GCHOICE:
          {
               GBOOL ingrp=(usrptr->substt == ST_GCHOICE);
               const FIELD *f=ingrp ? &gfields[editfld[usrnum]]
                                    : &fields[editfld[usrnum]];
               INT nc=nchoices(f);

               if (sameas(line,"?")) {
                    prfmsg(f->help);
                    showchoices(f);
                    break;
               }
               usrptr->substt=ingrp ? ST_GEDIT : ST_MENU;
               if (line[0] == '\0') {
                    if (ingrp) {
                         showgroup();
                    }
                    else {
                         showmenu();
                    }
               }
               else if ((n=atoi(line)) >= 1 && n <= nc
                     && strspn(line,"0123456789") == strlen(line)) {
                    applyvalue(f,choicekey(f,n-1));
               }
               else {
                    applyvalue(f,line);     /* the choice typed by name   */
               }
          }
          break;
     case ST_GROUPS:
          if (line[0] == '\0' || sameas(line,"?")) {
               showgroups();
          }
          else if (sameas(line,"X")) {
               usrptr->substt=ST_MENU;
               showmenu();
          }
          else if (sameas(line,"N")) {
               grptop[usrnum]+=GROWS;
               if (grptop[usrnum] >= ngrps) {
                    grptop[usrnum]=0;
               }
               showgroups();
          }
          else if (sameas(line,"H")) {
               prfmsg(HELPGRP);
          }
          else if (sameas(line,"A")) {
               if (ngrps >= MAXGRPS) {
                    sprintf(tvhint,"at most %d newsgroups can be linked",
                            MAXGRPS);
                    prfmsg(GBAD);
               }
               else {
                    usrptr->substt=ST_GADDNAME;
                    prfmsg(GASKNAME);
               }
          }
          else if ((n=atoi(line)) >= 1 && n <= ngrps
                && strspn(line,"0123456789") == strlen(line)) {
               curgrp[usrnum]=n-1;
               usrptr->substt=ST_GEDIT;
               showgroup();
          }
          else {
               prfmsg(BADSEL);
          }
          break;
     case ST_GADDNAME:
          if (line[0] == '\0') {
               usrptr->substt=ST_GROUPS;
               showgroups();
               break;
          }
          if (!validgroup(line)) {
               stzcpy(tvhint,"letters, digits, dots and - + _ only; no spaces",
                      sizeof(tvhint));
               prfmsg(GBAD);
               prfmsg(GASKNAME);
               break;
          }
          if (grpfind(line) >= 0) {
               stzcpy(tvhint,"that newsgroup is already linked",
                      sizeof(tvhint));
               prfmsg(GBAD);
               prfmsg(GASKNAME);
               break;
          }
          stzcpy(addname[usrnum],line,RECKEYSZ);
          stzcpy(tvgname,line,sizeof(tvgname));
          usrptr->substt=ST_GADDFOR;
          prfmsg(GASKFOR);
          break;
     case ST_GADDFOR:
          {
               USHORT fid;
               struct bngrp *g;

               if (line[0] == '\0') {
                    usrptr->substt=ST_GROUPS;
                    showgroups();
                    break;
               }
               if ((fid=getfid(line)) == EMLID) {
                    stzcpy(tvhint,"there is no forum by that name",
                           sizeof(tvhint));
                    prfmsg(GBAD);
                    prfmsg(GASKFOR);
                    break;
               }
               if (ngrps >= MAXGRPS || grpfind(addname[usrnum]) >= 0) {
                    usrptr->substt=ST_GROUPS;
                    showgroups();
                    break;
               }
               g=&grps[ngrps];
               setmem((CHAR *)g,sizeof(*g),0);
               stzcpy(g->name,addname[usrnum],sizeof(g->name));
               stzcpy(g->forum,getfnm(fid),sizeof(g->forum));
               g->doimp='Y';
               g->doexp='Y';
               g->next=0;
               ngrps++;
               grpsave(ngrps-1,NULL);
               audit("NEWSGROUPS SETTINGS","%s linked %.30s to %.15s",
                     usaptr->userid,g->name,g->forum);
               stzcpy(tvgname,g->name,sizeof(tvgname));
               stzcpy(tvgforum,g->forum,sizeof(tvgforum));
               prfmsg(GADDED);
               curgrp[usrnum]=ngrps-1;
               usrptr->substt=ST_GEDIT;
               showgroup();
          }
          break;
     case ST_GEDIT:
          i=curgrp[usrnum];
          if (i < 0 || i >= ngrps) {
               usrptr->substt=ST_GROUPS;
               showgroups();
               break;
          }
          if (line[0] == '\0' || sameas(line,"?")) {
               showgroup();
          }
          else if (sameas(line,"X")) {
               usrptr->substt=ST_GROUPS;
               showgroups();
          }
          else if (sameas(line,"D")) {
               stzcpy(tvgname,grps[i].name,sizeof(tvgname));
               usrptr->substt=ST_GDELASK;
               prfmsg(GDELASK);
          }
          else if (sameas(line,"H")) {
               prfmsg(HELPGRP);
          }
          else if ((n=atoi(line)) >= 1 && n <= NGFIELDS
                && strspn(line,"0123456789") == strlen(line)) {
               editfld[usrnum]=n-1;
               startedit(&gfields[n-1],n,ST_GVALUE,ST_GCHOICE);
          }
          else {
               prfmsg(BADSEL);
          }
          break;
     case ST_GDELASK:
          i=curgrp[usrnum];
          usrptr->substt=ST_GROUPS;
          if ((sameas(line,"Y") || sameas(line,"YES")) && i >= 0 && i < ngrps) {
               stzcpy(tvgname,grps[i].name,sizeof(tvgname));
               audit("NEWSGROUPS SETTINGS","%s removed %.40s",usaptr->userid,
                     grps[i].name);
               grpremove(i);
               prfmsg(GDELETED);
          }
          grptop[usrnum]=0;
          showgroups();
          break;
     }
     /* finish with the prompt unless the screen just shown has it */
     if (!menushown) {
          switch (usrptr->substt) {
          case ST_MENU:
               prfmsg(PROMPT);
               break;
          case ST_GROUPS:
               prfmsg(GRPFTR);
               break;
          case ST_GEDIT:
               prfmsg(GEDFTR);
               break;
          }
     }
     outprf(usrnum);
     rstmbk();
     return(1);
}

/*
 * Start editing a setting: a list of choices for settings that have a
 * fixed set of values, otherwise a prompt for the new value.
 */
static VOID
startedit(
const FIELD *f,
INT num,                           /*   number shown for it                */
INT valstate,                      /*   state for typing a value           */
INT chstate)                       /*   state for picking a choice         */
{
     sprintf(tvrownum,"%d",num);
     stzcpy(tvrowname,f->name,sizeof(tvrowname));
     fieldvalue(f,tvrowval,sizeof(tvrowval));
     stzcpy(tvhint,f->hint,sizeof(tvhint));
     if (nchoices(f) > 0) {
          usrptr->substt=chstate;
          showchoices(f);
     }
     else {
          usrptr->substt=valstate;
          prfmsg(ASKVAL);
     }
}

/*
 * Settings with a fixed set of values list them, numbered, each with a
 * short description; the Sysop types a number or the name.  choicekey()
 * is the value setfield() accepts, in the order of the list.
 */
static const CHAR *seckeys[]={"NONE","STARTTLS","TLS"};
static const CHAR *secdesc[]={
     "no encryption (usually port 119)",
     "plain connect, then encrypt (port 119)",
     "encrypted from the start (usually port 563)"
};
static const CHAR *ynkeys[]={"YES","NO"};

static INT                         /*   0 = not a list setting             */
nchoices(
const FIELD *f)
{
     switch (f->type) {
     case FT_SEC:
          return((INT)(sizeof(seckeys)/sizeof(seckeys[0])));
     case FT_YN:
          return(2);
     }
     return(0);
}

static const CHAR *
choicekey(
const FIELD *f,
INT i)
{
     return(f->type == FT_SEC ? seckeys[i] : ynkeys[i]);
}

static const CHAR *
choicedesc(
const FIELD *f,
INT i)
{
     return(f->type == FT_SEC ? secdesc[i] : "");
}

static VOID
showchoices(                       /* list a setting's choices, and prompt */
const FIELD *f)
{
     INT i,nc=nchoices(f);

     prfmsg(CHOICES);
     for (i=0 ; i < nc ; i++) {
          sprintf(tvchnum,"%d",i+1);
          stzcpy(tvchname,choicekey(f,i),sizeof(tvchname));
          stzcpy(tvchdesc,choicedesc(f,i),sizeof(tvchdesc));
          prfmsg(CHROW);
     }
     sprintf(tvchnum,"%d",nc);
     prfmsg(CHOICEP);
}

static VOID
applyvalue(                        /* set, save and report a new value     */
const FIELD *f,
const CHAR *value)
{
     CHAR oldkey[RECKEYSZ];
     INT i=curgrp[usrnum];

     if (f->rec == REC_GRP) {
          if (i < 0 || i >= ngrps) {
               return;
          }
          stzcpy(oldkey,grps[i].recid,sizeof(oldkey));
     }
     if (!setfield(f,value)) {
          stzcpy(tvhint,f->hint,sizeof(tvhint));
          prfmsg(BADVAL);
          if (f->rec == REC_GRP) {
               showgroup();
          }
          else {
               showmenu();
          }
          return;
     }
     stzcpy(tvrowname,f->name,sizeof(tvrowname));
     if (f->rec == REC_GRP) {
          grpsave(i,oldkey);
          audit("NEWSGROUPS SETTINGS","%s changed %.30s: %s",usaptr->userid,
                grps[i].name,f->name);
          prfmsg(SAVED);
          showgroup();
     }
     else {
          cfgsave();
          audit("NEWSGROUPS SETTINGS","%s changed %s",usaptr->userid,f->name);
          prfmsg(SAVED);
          curpage[usrnum]=pageof(editfld[usrnum]);
          showmenu();
     }
}

static const CHAR *                /*   why it does not apply, or NULL     */
unused(
const FIELD *f)
{
     if (f->rec == REC_CFG && f->off == offsetof(struct bncfg,verify)
      && cfg.security == BNSEC_NONE) {
          return("(not used without TLS)");
     }
     return(NULL);
}

static INT
pageof(                            /* which page a setting is on           */
INT fld)
{
     INT p,k;

     for (p=0 ; p < NPAGES ; p++) {
          for (k=0 ; k < pages[p].n ; k++) {
               if (pages[p].flds[k] == fld) {
                    return(p);
               }
          }
     }
     return(0);
}

static VOID
showmenu(VOID)                     /* the settings screen (current page)   */
{
     const PAGE *pg;
     INT i,k;

     if (curpage[usrnum] < 0 || curpage[usrnum] >= NPAGES) {
          curpage[usrnum]=0;
     }
     pg=&pages[curpage[usrnum]];
     setstatus();
     _snprintf(tvpage,sizeof(tvpage)-1,"%d of %d: %s",curpage[usrnum]+1,
               NPAGES,pg->title);
     tvpage[sizeof(tvpage)-1]='\0';
     prfmsg(MAINHDR);
     for (k=0 ; k < pg->n ; k++) {
          i=pg->flds[k];
          sprintf(tvrownum,"%2d",i+1);
          stzcpy(tvrowname,fields[i].name,sizeof(tvrowname));
          fieldvalue(&fields[i],tvrowval,sizeof(tvrowval));
          if (unused(&fields[i]) != NULL) {
               stzcpy(tvrowval,unused(&fields[i]),sizeof(tvrowval));
          }
          /* a long value is shortened on the menu (shown in full when edited) */
          if (strlen(tvrowval) > ROWVALW) {
               strcpy(tvrowval+ROWVALW-3,"...");
          }
          prfmsg(SETROW);
     }
     prfmsg(MAINFTR);
     prfmsg(PROMPT);
     menushown=TRUE;
}

static VOID
showgroups(VOID)                   /* the newsgroup list                   */
{
     INT i,shown=0;

     setstatus();
     prfmsg(GRPHDR);
     if (grptop[usrnum] < 0 || grptop[usrnum] >= ngrps) {
          grptop[usrnum]=0;
     }
     for (i=grptop[usrnum] ; i < ngrps && shown < GROWS ; i++,shown++) {
          sprintf(tvgnum,"%2d",i+1);
          stzcpy(tvgname,grps[i].name,sizeof(tvgname));
          if (strlen(tvgname) > 28) {
               strcpy(tvgname+25,"...");
          }
          stzcpy(tvgforum,grps[i].forum,sizeof(tvgforum));
          stzcpy(tvgimp,grps[i].doimp == 'Y' ? "YES" : "no",sizeof(tvgimp));
          stzcpy(tvgexp,grps[i].doexp == 'Y' ? "YES" : "no",sizeof(tvgexp));
          if (grps[i].lasterr[0] != '\0') {
               stzcpy(tvgstat,"PROBLEM",sizeof(tvgstat));
          }
          else if (grps[i].next <= 0) {
               stzcpy(tvgstat,"new",sizeof(tvgstat));
          }
          else {
               _snprintf(tvgstat,sizeof(tvgstat)-1,"next %I64d",grps[i].next);
               tvgstat[sizeof(tvgstat)-1]='\0';
          }
          prfmsg(GRPROW);
     }
     if (ngrps == 0) {
          prfmsg(GRPNONE);
     }
     prfmsg(GRPFTR);
     menushown=TRUE;
}

static VOID
showgroup(VOID)                    /* one newsgroup link's settings        */
{
     struct bngrp *g;
     INT k;

     if (curgrp[usrnum] < 0 || curgrp[usrnum] >= ngrps) {
          return;
     }
     g=&grps[curgrp[usrnum]];
     stzcpy(tvgname,g->name,sizeof(tvgname));
     if (g->lasterr[0] != '\0') {
          _snprintf(tvstatus,sizeof(tvstatus)-1,"PROBLEM: %.60s",g->lasterr);
     }
     else if (g->lastok != 0) {
          _snprintf(tvstatus,sizeof(tvstatus)-1,"ok; %ld imported, %ld posted "
                    "since linked",g->nimp,g->nexp);
     }
     else {
          _snprintf(tvstatus,sizeof(tvstatus)-1,"not read yet");
     }
     tvstatus[sizeof(tvstatus)-1]='\0';
     prfmsg(GEDHDR);
     for (k=0 ; k < NGFIELDS ; k++) {
          sprintf(tvrownum,"%2d",k+1);
          stzcpy(tvrowname,gfields[k].name,sizeof(tvrowname));
          fieldvalue(&gfields[k],tvrowval,sizeof(tvrowval));
          if (strlen(tvrowval) > ROWVALW) {
               strcpy(tvrowval+ROWVALW-3,"...");
          }
          prfmsg(SETROW);
     }
     prfmsg(GEDFTR);
     menushown=TRUE;
}

static VOID
showqueue(VOID)                    /* list posts waiting to go out         */
{
     time_t now=time(NULL);
     INT i,shown=0;

     setstatus();
     prfmsg(QHDR);
     for (i=0 ; i < noutq && shown < 12 ; i++,shown++) {   /* fits 24 lines */
          LONG mins=(LONG)((now-outq[i].queued)/60);

          stzcpy(tvqgrp,outq[i].groups,sizeof(tvqgrp));
          stzcpy(tvqsubj,outq[i].topic,sizeof(tvqsubj));
          if (mins < 60) {
               sprintf(tvqage,"%ldm",mins);
          }
          else {
               sprintf(tvqage,"%ldh%02ldm",mins/60,mins%60);
          }
          sprintf(tvqtries,"%d",outq[i].tries);
          stzcpy(tvqerr,outq[i].inflight ? "(posting now)"
                 : outq[i].lasterr[0] != '\0' ? outq[i].lasterr
                 : "(waiting)",19);        /* 18 characters fit the row */
          prfmsg(QROW);
     }
     if (noutq == 0) {
          prfmsg(QEMPTY);
     }
     else if (noutq > shown) {
          sprintf(tvstatus,"%d more not shown",noutq-shown);
          prfmsg(QMORE);
     }
}

/*
 * One-line summary for the screens: whether the gateway is on, how the
 * last check went, and what is waiting.
 */
static VOID
setstatus(VOID)
{
     CHAR state[96];
     time_t now=time(NULL);

     if (cfg.enabled != 'Y') {
          stzcpy(state,"gateway OFF",sizeof(state));
     }
     else if (!configured()) {
          _snprintf(state,sizeof(state)-1,"needs %s",missing());
     }
     else if (busy) {
          stzcpy(state,busyfetch ? "ON, checking now" : "ON, posting now",
                 sizeof(state));
     }
     else if (lastpoll == 0) {
          stzcpy(state,lastok ? "ON, not checked yet" : "ON, last try FAILED",
                 sizeof(state));
     }
     else if (!lastok) {
          _snprintf(state,sizeof(state)-1,"ON, FAILED: %.56s",lasterr);
     }
     else {
          _snprintf(state,sizeof(state)-1,"ON, checked %ldm ago (%d new)",
                    (LONG)((now-lastpoll)/60),lastnew);
     }
     state[sizeof(state)-1]='\0';
     stzcpy(tvstatus,state,sizeof(tvstatus));
     _snprintf(tvwait,sizeof(tvwait)-1,"%d to import, %d to post",
               npend+(cur.active ? 1 : 0),noutq);
     tvwait[sizeof(tvwait)-1]='\0';
}

static CHAR *
fieldbase(                         /* the record a field belongs to        */
const FIELD *f)
{
     if (f->rec == REC_GRP) {
          return((CHAR *)&grps[curgrp[usrnum]]);
     }
     return((CHAR *)&cfg);
}

static VOID
fieldvalue(                        /* a setting's value, for display       */
const FIELD *f,
CHAR *dst,
size_t dstsz)
{
     const CHAR *p=fieldbase(f)+f->off;

     switch (f->type) {
     case FT_YN:
          stzcpy(dst,*p == 'Y' ? "YES" : "NO",dstsz);
          break;
     case FT_NUM:
          _snprintf(dst,dstsz-1,"%d",*(const INT *)p);
          dst[dstsz-1]='\0';
          break;
     case FT_WRAP:
          if (*(const INT *)p == 0) {
               stzcpy(dst,"0 (off)",dstsz);
          }
          else {
               _snprintf(dst,dstsz-1,"%d",*(const INT *)p);
               dst[dstsz-1]='\0';
          }
          break;
     case FT_BACK:
          if (*(const INT *)p >= BNW_BACKALL) {
               stzcpy(dst,"ALL (every article the server has)",dstsz);
          }
          else {
               _snprintf(dst,dstsz-1,"%d",*(const INT *)p);
               dst[dstsz-1]='\0';
          }
          break;
     case FT_ART:
          if (*(const __int64 *)p <= 0) {
               stzcpy(dst,"0 (start with the newest articles)",dstsz);
          }
          else {
               _snprintf(dst,dstsz-1,"%I64d",*(const __int64 *)p);
               dst[dstsz-1]='\0';
          }
          break;
     case FT_SEC:
          stzcpy(dst,*p == BNSEC_TLS ? "TLS" : *p == BNSEC_STARTTLS
                 ? "STARTTLS" : "NONE",dstsz);
          break;
     case FT_SECRET:
          stzcpy(dst,*p != '\0' ? "(set)" : "(not set)",dstsz);
          break;
     case FT_STR:
          if (*p == '\0' && f->off == offsetof(struct bncfg,domain)) {
               _snprintf(dst,dstsz-1,"(mail host: %s)",smtpHost());
               dst[dstsz-1]='\0';
               break;
          }
          /* fall through */
     default:
          stzcpy(dst,*p != '\0' ? p : "(blank)",dstsz);
          break;
     }
}

static GBOOL                       /*   FALSE if the value is not valid    */
setfield(                          /* apply a new value to a setting       */
const FIELD *f,
const CHAR *input)
{
     CHAR *p=fieldbase(f)+f->off;
     INT n;
     USHORT fid;

     switch (f->type) {
     case FT_YN:
          if (sameas(input,"Y") || sameas(input,"YES")) {
               *p='Y';
          }
          else if (sameas(input,"N") || sameas(input,"NO")) {
               *p='N';
          }
          else {
               return(FALSE);
          }
          return(TRUE);
     case FT_BACK:
          if (sameas(input,"ALL")) {
               *(INT *)p=BNW_BACKALL;
               return(TRUE);
          }
          /* fall through: otherwise a number in range */
     case FT_NUM:
     case FT_WRAP:
          n=atoi(input);
          if (strspn(input,"0123456789") != strlen(input) || strlen(input) > 9
           || n < f->min || n > f->max
           || (f->type == FT_WRAP && n != 0 && n < 40)) {
               return(FALSE);
          }
          *(INT *)p=n;
          return(TRUE);
     case FT_ART:
          if (strspn(input,"0123456789") != strlen(input)
           || strlen(input) > 18) {
               return(FALSE);
          }
          *(__int64 *)p=_atoi64(input);
          return(TRUE);
     case FT_SEC:
          if (sameas(input,"NONE")) {
               *p=BNSEC_NONE;
          }
          else if (sameas(input,"STARTTLS")) {
               *p=BNSEC_STARTTLS;
          }
          else if (sameas(input,"TLS") || sameas(input,"SSL")) {
               *p=BNSEC_TLS;
          }
          else {
               return(FALSE);
          }
          return(TRUE);
     case FT_GNAME:
          if (!validgroup(input)) {
               return(FALSE);
          }
          n=grpfind(input);
          if (n >= 0 && n != curgrp[usrnum]) {
               return(FALSE);      /* another link already has this name  */
          }
          setmem(p,(UINT)f->size,0);
          stzcpy(p,input,(INT)f->size);
          return(TRUE);
     case FT_FORUM:
          if ((fid=getfid(input)) == EMLID) {
               return(FALSE);
          }
          setmem(p,(UINT)f->size,0);
          stzcpy(p,getfnm(fid),(INT)f->size);
          return(TRUE);
     case FT_KEY:
          if (!sameas(input,"-") && (strchr(input,' ') != NULL
                                  || strlen(input) >= f->size)) {
               return(FALSE);
          }
          /* fall through: stored like any other text setting */
     default:                      /* text and secrets                    */
          if (sameas(input,"-")) {
               setmem(p,(UINT)f->size,0);
               return(TRUE);
          }
          if (strlen(input) >= f->size || strchr(input,'\r') != NULL) {
               return(FALSE);
          }
          setmem(p,(UINT)f->size,0);
          stzcpy(p,input,(INT)f->size);
          return(TRUE);
     }
}

/*
 * A newsgroup name as Usenet allows it (RFC 5536 3.1.4): components of
 * letters, digits, '+', '-' and '_', separated by single dots.
 */
static GBOOL
validgroup(
const CHAR *name)
{
     const CHAR *p;
     CHAR prev='.';

     if (name[0] == '\0' || strlen(name) >= RECKEYSZ) {
          return(FALSE);
     }
     for (p=name ; *p != '\0' ; p++) {
          if (*p == '.') {
               if (prev == '.') {
                    return(FALSE); /* empty component                     */
               }
          }
          else if (!isalnum((UCHAR)*p) && *p != '+' && *p != '-'
                && *p != '_') {
               return(FALSE);
          }
          prev=*p;
     }
     return(prev != '.');
}

VOID
bnhangup(VOID)                     /* user hung up: forget editor state    */
{
     editfld[usrnum]=0;
     curpage[usrnum]=0;
     curgrp[usrnum]=0;
     grptop[usrnum]=0;
     addname[usrnum][0]='\0';
}

/* ======================================================================== */
/*   Shutdown and helpers                                                    */
/* ======================================================================== */

VOID
bnshutdown(VOID)                   /* clean BBS shutdown                   */
{
     /*
      * Stop the timer and the worker.  An article being imported stays in
      * the spool and is finished after the restart (the Message-ID record
      * keeps it from going in twice); a post being sent stays queued.
      */
     running=FALSE;
     bnwStop(5000);
     if (cur.again && gmerqopn(workarea)) {
          clsgmerq(workarea);
     }
     SecureZeroMemory(&cfg,sizeof(cfg));
     if (cfgbb != NULL) {
          dfaClose(cfgbb);
          cfgbb=NULL;
     }
     if (midbb != NULL) {
          dfaClose(midbb);
          midbb=NULL;
     }
     if (bnmb != NULL) {
          clsmsg(bnmb);
          bnmb=NULL;
     }
}

static VOID
audit(                             /* Audit Trail entry                    */
const CHAR *brief,                 /*   short title (at most 32 chars)     */
const CHAR *fmt,                   /*   detail format (a literal)          */
...)
{
     CHAR detail[120],title[33];
     va_list ap;

     /*
      * The detail is formatted here and passed as "%s": the Audit Trail
      * treats its detail as a format string, and runtime text containing
      * '%' would otherwise be misread.  Lengths are capped well below the
      * limits that halt the board (128 detail, 33 title).
      */
     va_start(ap,fmt);
     _vsnprintf(detail,sizeof(detail)-1,fmt,ap);
     va_end(ap);
     detail[sizeof(detail)-1]='\0';
     stzcpy(title,brief,sizeof(title));
     shocst(title,"%s",detail);
}

static VOID
sanitize(                          /* remove control characters in place   */
CHAR *s)
{
     for ( ; *s != '\0' ; s++) {
          if ((UCHAR)*s < 0x20) {
               *s=' ';
          }
     }
}

VOID EXPORT
end_snbetnws(VOID)                 /* marks the end of the code for crash  */
{                                  /* report (GALEXCEP.OUT) analysis       */
}
