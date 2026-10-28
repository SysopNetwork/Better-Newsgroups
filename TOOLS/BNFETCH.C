/***************************************************************************
 *                                                                         *
 *   BNFETCH.C                                                             *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   Command-line test tool.  Runs the module's own news code (NNTP,      *
 *   article decoding, spool files, article building) against a real news *
 *   server, without a BBS.  Settings come from BNFETCH.INI next to the   *
 *   program (see BNFETCH.INI.EXAMPLE).                                    *
 *                                                                         *
 *     BNFETCH test                  log in and open each group            *
 *     BNFETCH fetch [group] [n]     read the last n articles (default 5)  *
 *     BNFETCH session [n]           one full worker session, backfill n   *
 *     BNFETCH show <spool id>       print a spooled article's text        *
 *     BNFETCH build                 print a sample outgoing article       *
 *     BNFETCH post <group> YES      post the sample article for real      *
 *                                                                         *
 *   Add -v anywhere to see the whole server conversation.                 *
 *                                                                         *
 ***************************************************************************/

#include "BNWORK.H"
#include "BNART.H"
#include "SBMAIL.H"
#include "SBUTIL.H"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

typedef struct ini {
     CHAR host[256];
     INT port;
     INT security;
     BOOL verify;
     CHAR user[256];
     CHAR pass[256];
     CHAR groups[16][BNSP_GROUPSZ];
     INT ngroups;
     CHAR from[256];
} INI;

static INI ini;
static CHAR exedir[MAX_PATH];
static CHAR spooldir[MAX_PATH];
static BOOL verbose=FALSE;

static BOOL loadini(VOID);
static VOID say(VOID *ctx, const CHAR *line);
static INT cmdtest(VOID);
static INT cmdfetch(const CHAR *group, INT n);
static INT cmdsession(INT n);
static INT cmdshow(const CHAR *sid);
static INT cmdbuild(BOOL quiet, CHAR **art, size_t *len, const CHAR *group);
static INT cmdpost(const CHAR *group);
static VOID summary(const CHAR *sid);

static const CHAR sample[]=
     "\x1b[1;37mHello from the Better Newsgroups test tool.\x1b[0m\r"
     "\r"
     "This line has CP437 box drawing: \xC4\xC4\xC4 and a pound sign \x9C.\r"
     ".A line starting with a dot (sent doubled, read back single).\r"
     "\r"
     "Last line.\r";

INT
main(
INT argc,
CHAR **argv)
{
     WSADATA wsa;
     CHAR *args[8];
     INT nargs=0,i,rc;
     CHAR *p;

     for (i=1 ; i < argc ; i++) {
          if (_stricmp(argv[i],"-v") == 0) {
               verbose=TRUE;
          }
          else if (nargs < 8) {
               args[nargs++]=argv[i];
          }
     }
     GetModuleFileNameA(NULL,exedir,sizeof(exedir));
     if ((p=strrchr(exedir,'\\')) != NULL) {
          p[1]='\0';
     }
     _snprintf(spooldir,sizeof(spooldir)-1,"%sSPOOL\\",exedir);
     CreateDirectoryA(spooldir,NULL);

     if (nargs == 0) {
          printf("BNFETCH test | fetch [group] [n] | session [n] | show <sid> |"
                 " build | post <group> YES   (-v = transcript)\n");
          return(2);
     }
     if (_stricmp(args[0],"build") == 0) {
          return(cmdbuild(FALSE,NULL,NULL,"alt.test"));
     }
     if (_stricmp(args[0],"show") == 0) {
          return(nargs > 1 ? cmdshow(args[1]) : 2);
     }
     if (!loadini()) {
          printf("BNFETCH.INI not found or incomplete (see BNFETCH.INI.EXAMPLE)\n");
          return(2);
     }
     WSAStartup(MAKEWORD(2,2),&wsa);
     if (_stricmp(args[0],"test") == 0) {
          rc=cmdtest();
     }
     else if (_stricmp(args[0],"fetch") == 0) {
          rc=cmdfetch(nargs > 1 ? args[1] : ini.groups[0],
                      nargs > 2 ? atoi(args[2]) : 5);
     }
     else if (_stricmp(args[0],"session") == 0) {
          rc=cmdsession(nargs > 1 ? atoi(args[1]) : 5);
     }
     else if (_stricmp(args[0],"post") == 0) {
          if (nargs < 3 || strcmp(args[2],"YES") != 0) {
               printf("post publishes to Usenet: BNFETCH post <group> YES\n");
               rc=2;
          }
          else {
               rc=cmdpost(args[1]);
          }
     }
     else {
          printf("unknown command\n");
          rc=2;
     }
     WSACleanup();
     return(rc);
}

static INT
cmdtest(VOID)
{
     BNNNTP s;
     INT i;

     bnnInit(&s,verbose ? say : NULL,NULL);
     if (!bnnConnect(&s,ini.host,ini.port,ini.security,ini.verify,30)) {
          printf("CONNECT FAILED: %s\n",s.err);
          return(1);
     }
     printf("connected to %s (%s), %s\n",ini.host,s.net.peer,
            s.net.tls ? s.net.tlsinfo : "not encrypted");
     printf("greeting/mode: posting %s\n",s.canpost ? "allowed" : "not allowed");
     if (!bnnAuth(&s,ini.user,ini.pass)) {
          printf("LOGIN FAILED: %s\n",s.err);
          bnnClose(&s);
          return(1);
     }
     printf("login OK (%s)\n",ini.user[0] != '\0' ? ini.user : "no login");
     for (i=0 ; i < ini.ngroups ; i++) {
          BNARTNUM c,f,l;

          if (bnnGroup(&s,ini.groups[i],&c,&f,&l)) {
               printf("  %-30s %I64d articles, %I64d-%I64d\n",ini.groups[i],c,f,l);
          }
          else {
               printf("  %-30s FAILED: %s\n",ini.groups[i],s.err);
          }
     }
     bnnQuit(&s);
     return(0);
}

static INT
cmdfetch(
const CHAR *group,
INT n)
{
     BNNNTP s;
     BNARTNUM c,f,l,a;
     INT got=0;

     bnnInit(&s,verbose ? say : NULL,NULL);
     if (!bnnConnect(&s,ini.host,ini.port,ini.security,ini.verify,30)
      || !bnnAuth(&s,ini.user,ini.pass)) {
          printf("FAILED: %s\n",s.err);
          bnnClose(&s);
          return(1);
     }
     if (!bnnGroup(&s,group,&c,&f,&l)) {
          printf("GROUP FAILED: %s\n",s.err);
          bnnQuit(&s);
          return(1);
     }
     printf("%s: %I64d articles, %I64d-%I64d; reading the last %d\n",group,c,
            f,l,n);
     for (a=l-n+1 ; a <= l ; a++) {
          CHAR *buf,sid[BNSP_IDSZ];
          size_t len;
          BOOL trunc;
          INT code;

          if (a < f) {
               continue;
          }
          code=bnnArticle(&s,a,&buf,&len,64*1024,&trunc);
          if (code < 0) {
               printf("  %I64d: connection lost: %s\n",a,s.err);
               return(1);
          }
          if (code != 220) {
               printf("  %I64d: %s\n",a,s.reply);
               continue;
          }
          if (bnwSpoolArticle(spooldir,group,a,buf,len,trunc,79,ini.host,sid)) {
               printf("  %I64d -> spool %s (%u bytes%s)\n",a,sid,(unsigned)len,
                      trunc ? ", truncated" : "");
               summary(sid);
               got++;
          }
          else {
               printf("  %I64d: could not spool\n",a);
          }
          free(buf);
     }
     bnnQuit(&s);
     printf("%d article(s) spooled in %s\n",got,spooldir);
     return(0);
}

static INT
cmdsession(
INT n)
{
     BNJOB *job;
     BNNNTP s;
     INT i;

     if ((job=(BNJOB *)calloc(1,sizeof(BNJOB))) == NULL) {
          return(1);
     }
     sbStrCopy(job->host,ini.host,sizeof(job->host));
     job->port=ini.port;
     job->security=ini.security;
     job->verify=ini.verify;
     sbStrCopy(job->user,ini.user,sizeof(job->user));
     sbStrCopy(job->pass,ini.pass,sizeof(job->pass));
     job->timeout=30;
     sbStrCopy(job->spooldir,spooldir,sizeof(job->spooldir));
     _snprintf(job->logfile,sizeof(job->logfile)-1,"%sBNFETCH.LOG",exedir);
     job->maxbytes=64L*1024L;
     job->maxper=200;
     job->backfill=n;
     job->wrapcol=79;
     job->maxnew=BNW_MAXNEW;
     job->dofetch=TRUE;
     for (i=0 ; i < ini.ngroups ; i++) {
          sbStrCopy(job->grp[i].name,ini.groups[i],BNSP_GROUPSZ);
          job->grp[i].next=0;
     }
     job->ngrp=ini.ngroups;
     bnnInit(&s,NULL,NULL);
     bnwRun(job,&s,NULL);
     printf("session %s%s%s%s; %d new article(s); transcript in %s\n",
            job->ok ? "OK" : "FAILED at ",job->ok ? "" : job->stage,
            job->err[0] != '\0' ? ": " : "",job->err,job->nnew,job->logfile);
     for (i=0 ; i < job->ngrp ; i++) {
          BNWGRP *g=&job->grp[i];
          printf("  %-28s %s first %I64d last %I64d -> next %I64d, fetched %d,"
                 " missing %d, bad %d%s%s\n",g->name,g->ok ? "ok" : "FAILED",
                 g->first,g->last,g->newnext,g->nfetched,g->nmissing,g->nbad,
                 g->err[0] != '\0' ? " - " : "",g->err);
     }
     for (i=0 ; i < job->nnew ; i++) {
          summary(job->newsid[i]);
     }
     bnwWipe(job);
     free(job);
     return(0);
}

static INT
cmdshow(
const CHAR *sid)
{
     CHAR path[MAX_PATH];
     CHAR *text;
     LONG len;

     summary(sid);
     bnspoolPath(path,spooldir,sid,"TXT");
     if ((text=sbReadFile(path,&len,4L*1024L*1024L)) == NULL) {
          printf("no text for %s\n",sid);
          return(1);
     }
     fwrite(text,1,(size_t)len,stdout);
     printf("\n-- end --\n");
     free(text);
     return(0);
}

static INT
cmdbuild(
BOOL quiet,
CHAR **out,
size_t *outlen,
const CHAR *group)
{
     BNPOST p;
     CHAR *art;
     size_t len;

     memset(&p,0,sizeof(p));
     sbStrCopy(p.newsgroups,group,sizeof(p.newsgroups));
     sbStrCopy(p.fromname,"Better Newsgroups Test",sizeof(p.fromname));
     sbStrCopy(p.fromadr,ini.from[0] != '\0' ? ini.from : "test@bbs.invalid",
               sizeof(p.fromadr));
     sbStrCopy(p.subject,"Better Newsgroups test \x9C",sizeof(p.subject));
     sbRfc5322Date(p.date,sizeof(p.date));
     bnartMakeMsgId(p.msgid,sizeof(p.msgid),1,2,"bbs.invalid");
     sbStrCopy(p.useragent,"Better Newsgroups (BNFETCH test tool)",
               sizeof(p.useragent));
     if ((art=bnartBuild(&p,sample,"Sent by the BNFETCH test tool",&len))
         == NULL) {
          printf("out of memory\n");
          return(1);
     }
     if (!quiet) {
          fwrite(art,1,len,stdout);
     }
     if (out != NULL) {
          *out=art;
          *outlen=len;
     }
     else {
          free(art);
     }
     return(0);
}

static INT
cmdpost(
const CHAR *group)
{
     BNNNTP s;
     CHAR *art;
     size_t len;
     INT code;

     if (cmdbuild(TRUE,&art,&len,group) != 0) {
          return(1);
     }
     bnnInit(&s,verbose ? say : NULL,NULL);
     if (!bnnConnect(&s,ini.host,ini.port,ini.security,ini.verify,30)
      || !bnnAuth(&s,ini.user,ini.pass)) {
          printf("FAILED: %s\n",s.err);
          free(art);
          bnnClose(&s);
          return(1);
     }
     code=bnnPost(&s,art,len);
     printf("POST -> %d %s\n",code,code < 0 ? s.err : s.reply);
     free(art);
     bnnQuit(&s);
     return(code == 240 ? 0 : 1);
}

static VOID
summary(                           /* one spooled article, briefly        */
const CHAR *sid)
{
     CHAR path[MAX_PATH],when[64];
     BNRDY r;
     time_t t;
     INT i;

     bnspoolPath(path,spooldir,sid,"RDY");
     if (!bnspoolRead(path,&r)) {
          printf("    (no .RDY for %s)\n",sid);
          return;
     }
     when[0]='\0';
     if (bnartParseDate(r.date,&t)) {
          strftime(when,sizeof(when),"%Y-%m-%d %H:%M local",localtime(&t));
     }
     printf("    #%I64d %s\n      From: %s <%s>\n      Subj: %s\n"
            "      Date: %s  [%s]\n      Id:   <%s>\n",r.artnum,r.group,
            r.fromname,r.from,r.subject,r.date,when[0] ? when : "UNREADABLE",
            r.msgid);
     if (r.npar > 0) {
          printf("      Root: <%s>\n",r.root);
          for (i=0 ; i < r.npar ; i++) {
               printf("      Ref%d: <%s>\n",i+1,r.parents[i]);
          }
     }
     if (r.oversize || r.natt > 0) {
          printf("      oversize=%d attachments=%d\n",r.oversize,r.natt);
     }
}

static VOID
say(                               /* transcript to the screen            */
VOID *ctx,
const CHAR *line)
{
     (VOID)ctx;
     printf("    %s\n",line);
}

static BOOL
loadini(VOID)
{
     CHAR path[MAX_PATH],line[512],*eq,*v;
     FILE *fp;

     memset(&ini,0,sizeof(ini));
     ini.port=119;
     ini.verify=TRUE;
     _snprintf(path,sizeof(path)-1,"%sBNFETCH.INI",exedir);
     if ((fp=fopen(path,"r")) == NULL) {
          return(FALSE);
     }
     while (fgets(line,sizeof(line),fp) != NULL) {
          line[strcspn(line,"\r\n")]='\0';
          if (line[0] == ';' || (eq=strchr(line,'=')) == NULL) {
               continue;
          }
          *eq='\0';
          v=eq+1;
          if (_stricmp(line,"host") == 0) {
               sbStrCopy(ini.host,v,sizeof(ini.host));
          }
          else if (_stricmp(line,"port") == 0) {
               ini.port=atoi(v);
          }
          else if (_stricmp(line,"security") == 0) {
               ini.security=(_stricmp(v,"tls") == 0) ? BNSEC_TLS
                          : (_stricmp(v,"starttls") == 0) ? BNSEC_STARTTLS
                          : BNSEC_NONE;
          }
          else if (_stricmp(line,"verify") == 0) {
               ini.verify=(toupper((UCHAR)v[0]) != 'N');
          }
          else if (_stricmp(line,"user") == 0) {
               sbStrCopy(ini.user,v,sizeof(ini.user));
          }
          else if (_stricmp(line,"pass") == 0) {
               sbStrCopy(ini.pass,v,sizeof(ini.pass));
          }
          else if (_stricmp(line,"from") == 0) {
               sbStrCopy(ini.from,v,sizeof(ini.from));
          }
          else if (_stricmp(line,"group") == 0 && ini.ngroups < 16) {
               sbStrCopy(ini.groups[ini.ngroups++],v,BNSP_GROUPSZ);
          }
     }
     fclose(fp);
     if (getenv("BNFETCH_PASS") != NULL) {
          sbStrCopy(ini.pass,getenv("BNFETCH_PASS"),sizeof(ini.pass));
     }
     return(ini.host[0] != '\0' && ini.ngroups > 0);
}
