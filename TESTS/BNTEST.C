/***************************************************************************
 *                                                                         *
 *   BNTEST.C                                                              *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   Unit tests for the parts that need no BBS and no network: NNTP        *
 *   helpers, article building, references, dates, spool files.           *
 *   Exit code 0 = every check passed.                                     *
 *                                                                         *
 ***************************************************************************/

#include "BNWORK.H"
#include "BNART.H"
#include "SBUTIL.H"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static INT nchk=0,nfail=0;

#define CHECK(cond,what) check((cond) ? TRUE : FALSE,what,__LINE__)

static VOID
check(
BOOL ok,
const CHAR *what,
INT line)
{
     nchk++;
     if (!ok) {
          nfail++;
          printf("FAIL line %d: %s\n",line,what);
     }
}

static VOID
testnntp(VOID)
{
     BNARTNUM c,f,l;
     CHAR *s;
     size_t n;

     CHECK(bnnParseGroup("211 4362 39704 44065 alt.bbs",&c,&f,&l)
           && c == 4362 && f == 39704 && l == 44065,"211 parsed");
     CHECK(bnnParseGroup("211 0 1 0 alt.empty",&c,&f,&l) && l < f,
           "empty group: last below first");
     CHECK(!bnnParseGroup("411 no such group",&c,&f,&l),"411 is not a 211");

     /* dot-stuffing: leading dots doubled, LF made CRLF, terminator */
     s=bnnDotStuff(".a\n..b\nc",8,&n);
     CHECK(s != NULL && strcmp(s,"..a\r\n...b\r\nc\r\n.\r\n") == 0,
           "dot-stuffing");
     free(s);
     s=bnnDotStuff("x\r\n.\r\n",6,&n);
     CHECK(s != NULL && strcmp(s,"x\r\n..\r\n.\r\n") == 0,
           "a lone dot line is never taken as the end");
     free(s);
}

static VOID
testdates(VOID)
{
     time_t t,u;

     CHECK(bnartParseDate("Mon, 19 Oct 2026 14:01:11 +0000",&t),"date read");
     CHECK(bnartParseDate("19 Oct 2026 07:01:11 -0700",&u) && u == t,
           "zone offset applied (and day name optional)");
     CHECK(bnartParseDate("Mon, 19 Oct 2026 10:01:11 EDT",&u) && u == t,
           "named zone");
     CHECK(bnartParseDate("Mon, 19 Oct 26 14:01 GMT",&u) && u == t-11,
           "2-digit year, no seconds");
     CHECK(!bnartParseDate("yesterday",&u),"junk refused");
     CHECK(!bnartParseDate("Mon, 32 Oct 2026 14:01:11 +0000",&u),
           "impossible day refused");
}

static VOID
testrefs(VOID)
{
     const CHAR *h1="From: a@b\r\nReferences: <r1@x> <r2@x>\r\n <r3@x>"
                    " <r4@x>\r\nSubject: s\r\n";
     const CHAR *h2="From: a@b\r\nIn-Reply-To: <p@x>\r\n";
     const CHAR *h3="From: a@b\r\nSubject: new\r\n";
     CHAR root[BN_MIDSZ],par[BN_MAXPAR][BN_MIDSZ];
     INT n;

     n=bnartRefs(h1,strlen(h1),root,par);
     CHECK(n == 3 && strcmp(root,"r1@x") == 0 && strcmp(par[0],"r4@x") == 0
           && strcmp(par[2],"r2@x") == 0,"References: root, nearest first, "
           "folded header");
     n=bnartRefs(h2,strlen(h2),root,par);
     CHECK(n == 1 && strcmp(par[0],"p@x") == 0,"In-Reply-To fallback");
     n=bnartRefs(h3,strlen(h3),root,par);
     CHECK(n == 0 && root[0] == '\0',"no references");
}

static VOID
testbuild(VOID)
{
     BNPOST p;
     CHAR *a,enc[256];
     size_t len;

     bnartEncodeWord("plain",enc,sizeof(enc));
     CHECK(strcmp(enc,"plain") == 0,"ASCII header text left alone");
     bnartEncodeWord("caf\x82",enc,sizeof(enc));
     CHECK(strncmp(enc,"=?UTF-8?B?",10) == 0,"CP437 accent encoded");

     memset(&p,0,sizeof(p));
     strcpy(p.newsgroups,"alt.test");
     strcpy(p.fromname,"Sysop, The");
     strcpy(p.fromadr,"sysop@bbs.example.com");
     strcpy(p.subject,"Re: hi");
     strcpy(p.date,"Mon, 19 Oct 2026 14:01:11 +0000");
     strcpy(p.msgid,"1.2.3.snbetnws@bbs.example.com");
     strcpy(p.root,"root@x");
     strcpy(p.parent,"par@x");
     a=bnartBuild(&p,"\x1b[1;37mHi\x1b[0m\r.dot\r",NULL,&len);
     CHECK(a != NULL,"article built");
     if (a == NULL) {
          return;
     }
     CHECK(strstr(a,"From: \"Sysop, The\" <sysop@bbs.example.com>\r\n")
           != NULL,"display name with a comma is quoted");
     CHECK(strstr(a,"References: <root@x>\r\n <par@x>\r\n") != NULL,
           "References: root and parent, folded");
     CHECK(strstr(a,"Path:") == NULL,"no Path header (server adds it)");
     CHECK(strstr(a,"\r\n\r\nHi\r\n.dot\r\n") != NULL,
           "body: ANSI removed, CR lines made CRLF, dot not stuffed yet");
     CHECK(strstr(a,"\x1b") == NULL,"no escape bytes anywhere");
     free(a);

     /* a parent without a separate root gives one reference */
     p.root[0]='\0';
     a=bnartBuild(&p,"x",NULL,&len);
     CHECK(a != NULL && strstr(a,"References: <par@x>\r\n") != NULL,
           "single reference");
     free(a);
}

static VOID
testlongline(VOID)
{
     BNPOST p;
     CHAR *body,*a,*s;
     size_t len,i,maxl=0,cur=0;

     body=(CHAR *)malloc(3001);
     for (i=0 ; i < 3000 ; i++) {
          body[i]=(CHAR)(i%2 ? 0xC4 : 'a');   /* half 3-byte UTF-8 chars */
     }
     body[3000]='\0';
     memset(&p,0,sizeof(p));
     strcpy(p.newsgroups,"alt.test");
     strcpy(p.fromadr,"a@b");
     a=bnartBuild(&p,body,NULL,&len);
     for (s=a ; s != NULL && *s != '\0' ; s++) {
          if (*s == '\r') {
               if (cur > maxl) {
                    maxl=cur;
               }
               cur=0;
          }
          else if (*s != '\n') {
               cur++;
          }
     }
     CHECK(a != NULL && maxl <= BN_LINEMAX,"no line over 998 bytes");
     free(a);
     free(body);
}

static VOID
testspool(VOID)
{
     CHAR dir[MAX_PATH],path[MAX_PATH],sid[BNSP_IDSZ];
     const CHAR *art="From: Ann <ann@x.org>\r\nNewsgroups: alt.test\r\n"
                     "Subject: =?UTF-8?Q?caf=C3=A9?=\r\n"
                     "Date: Mon, 19 Oct 2026 14:01:11 +0000\r\n"
                     "Message-ID: <m1@x.org>\r\nReferences: <p1@x.org>\r\n"
                     "\r\nHello\r\n";
     BNRDY r;

     GetTempPathA(sizeof(dir),dir);
     strcat(dir,"BNTEST\\");
     CreateDirectoryA(dir,NULL);
     CHECK(bnwSpoolArticle(dir,"alt.test",42,art,strlen(art),FALSE,79,"h",sid),
           "article spooled");
     bnspoolPath(path,dir,sid,"RDY");
     CHECK(bnspoolRead(path,&r),"spool read back");
     CHECK(r.artnum == 42 && strcmp(r.msgid,"m1@x.org") == 0
           && strcmp(r.from,"ann@x.org") == 0
           && strcmp(r.fromname,"Ann") == 0
           && strcmp(r.subject,"caf\x82") == 0
           && r.npar == 1 && strcmp(r.parents[0],"p1@x.org") == 0,
           "spool fields (subject decoded to CP437)");
     bnspoolDelete(dir,sid);
     CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES,
           "spool files removed");
     RemoveDirectoryA(dir);
}

static VOID
teststart(VOID)
{
     BOOL ren=FALSE;

     /* group with articles 39704-44065 on the server */
     CHECK(bnwStartAt(0,39704,44065,50,&ren) == 44016 && !ren,
           "new group: the newest 50");
     CHECK(bnwStartAt(0,39704,44065,0,&ren) == 44066,
           "new group, backfill 0: only articles from now on");
     CHECK(bnwStartAt(0,39704,44065,9999999,&ren) == 39704,
           "backfill larger than the group: from the first article");
     CHECK(bnwStartAt(0,39704,44065,BNW_BACKALL,&ren) == 39704,
           "backfill ALL: from the first article");
     CHECK(bnwStartAt(0,1,3000000000,BNW_BACKALL,&ren) == 1,
           "backfill ALL with article numbers past 32 bits");
     CHECK(bnwStartAt(41000,39704,44065,BNW_BACKALL,&ren) == 41000 && !ren,
           "a backlog in progress continues where it left off");
     CHECK(bnwStartAt(50000,39704,44065,BNW_BACKALL,&ren) == 39704 && ren,
           "renumbered server: start over");
}

INT
main(VOID)
{
     teststart();
     testnntp();
     testdates();
     testrefs();
     testbuild();
     testlongline();
     testspool();
     printf("%d checks, %d failed\n",nchk,nfail);
     return(nfail == 0 ? 0 : 1);
}
