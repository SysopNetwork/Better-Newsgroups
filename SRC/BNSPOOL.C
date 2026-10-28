/***************************************************************************
 *                                                                         *
 *   BNSPOOL.C                                                             *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   Incoming spool files.  The .RDY file is plain "name=value" text, one *
 *   item per line, so a Sysop can look at a stuck article with Notepad.   *
 *                                                                         *
 *   No BBS SDK calls are made from this file.                             *
 *                                                                         *
 ***************************************************************************/

#include "BNSPOOL.H"
#include "SBUTIL.H"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static VOID putval(FILE *fp, const CHAR *name, const CHAR *val);
static VOID newsid(const CHAR *dir, CHAR *sid);

VOID
bnspoolPath(                       /* full path of one spool file          */
CHAR *dst,
const CHAR *dir,
const CHAR *sid,
const CHAR *ext)
{
     _snprintf(dst,MAX_PATH-1,"%s%s.%s",dir,sid,ext);
     dst[MAX_PATH-1]='\0';
}

BOOL                               /*   TRUE if both files were written    */
bnspoolWrite(                      /* store one decoded article            */
const CHAR *dir,
BNRDY *r,
const CHAR *text,
size_t textlen)
{
     CHAR txt[MAX_PATH],rdy[MAX_PATH],tmp[MAX_PATH];
     FILE *fp;
     INT i;

     newsid(dir,r->sid);
     bnspoolPath(txt,dir,r->sid,"TXT");
     bnspoolPath(rdy,dir,r->sid,"RDY");
     bnspoolPath(tmp,dir,r->sid,"RTM");

     if ((fp=fopen(txt,"wb")) == NULL) {
          return(FALSE);
     }
     if (textlen > 0 && fwrite(text,1,textlen,fp) != textlen) {
          fclose(fp);
          DeleteFileA(txt);
          return(FALSE);
     }
     if (fclose(fp) != 0) {
          DeleteFileA(txt);
          return(FALSE);
     }

     if ((fp=fopen(tmp,"w")) == NULL) {
          DeleteFileA(txt);
          return(FALSE);
     }
     fprintf(fp,"version=1\n");
     putval(fp,"group",r->group);
     fprintf(fp,"artnum=%I64d\n",r->artnum);
     putval(fp,"msgid",r->msgid);
     putval(fp,"root",r->root);
     for (i=0 ; i < r->npar && i < BN_MAXPAR ; i++) {
          putval(fp,"parent",r->parents[i]);
     }
     putval(fp,"from",r->from);
     putval(fp,"fromname",r->fromname);
     putval(fp,"replyto",r->replyto);
     putval(fp,"subject",r->subject);
     putval(fp,"date",r->date);
     fprintf(fp,"size=%ld\n",r->size);
     fprintf(fp,"oversize=%d\n",r->oversize ? 1 : 0);
     fprintf(fp,"natt=%d\n",r->natt);
     if (fclose(fp) != 0
      || !MoveFileExA(tmp,rdy,MOVEFILE_REPLACE_EXISTING)) {
          DeleteFileA(tmp);
          DeleteFileA(txt);
          return(FALSE);
     }
     return(TRUE);
}

BOOL                               /*   TRUE if read                       */
bnspoolRead(                       /* load one .RDY file                   */
const CHAR *path,
BNRDY *r)
{
     FILE *fp;
     CHAR line[1024],*eq,*val;
     const CHAR *fname,*dot;

     memset(r,0,sizeof(*r));
     fname=strrchr(path,'\\');
     fname=(fname != NULL) ? fname+1 : path;
     if ((dot=strchr(fname,'.')) == NULL || dot-fname >= BNSP_IDSZ) {
          return(FALSE);
     }
     memcpy(r->sid,fname,dot-fname);
     r->sid[dot-fname]='\0';
     if ((fp=fopen(path,"r")) == NULL) {
          return(FALSE);
     }
     while (fgets(line,sizeof(line),fp) != NULL) {
          line[strcspn(line,"\r\n")]='\0';
          if ((eq=strchr(line,'=')) == NULL) {
               continue;
          }
          *eq='\0';
          val=eq+1;
          if (_stricmp(line,"group") == 0) {
               sbStrCopy(r->group,val,sizeof(r->group));
          }
          else if (_stricmp(line,"artnum") == 0) {
               r->artnum=_atoi64(val);
          }
          else if (_stricmp(line,"msgid") == 0) {
               sbStrCopy(r->msgid,val,sizeof(r->msgid));
          }
          else if (_stricmp(line,"root") == 0) {
               sbStrCopy(r->root,val,sizeof(r->root));
          }
          else if (_stricmp(line,"parent") == 0) {
               if (r->npar < BN_MAXPAR) {
                    sbStrCopy(r->parents[r->npar++],val,BN_MIDSZ);
               }
          }
          else if (_stricmp(line,"from") == 0) {
               sbStrCopy(r->from,val,sizeof(r->from));
          }
          else if (_stricmp(line,"fromname") == 0) {
               sbStrCopy(r->fromname,val,sizeof(r->fromname));
          }
          else if (_stricmp(line,"replyto") == 0) {
               sbStrCopy(r->replyto,val,sizeof(r->replyto));
          }
          else if (_stricmp(line,"subject") == 0) {
               sbStrCopy(r->subject,val,sizeof(r->subject));
          }
          else if (_stricmp(line,"date") == 0) {
               sbStrCopy(r->date,val,sizeof(r->date));
          }
          else if (_stricmp(line,"size") == 0) {
               r->size=atol(val);
          }
          else if (_stricmp(line,"oversize") == 0) {
               r->oversize=(atoi(val) != 0);
          }
          else if (_stricmp(line,"natt") == 0) {
               r->natt=atoi(val);
          }
     }
     fclose(fp);
     return(r->group[0] != '\0');
}

VOID
bnspoolDelete(                     /* remove one article's spool files     */
const CHAR *dir,
const CHAR *sid)
{
     CHAR path[MAX_PATH];

     /* the .RDY first: without it the .TXT is never picked up again */
     bnspoolPath(path,dir,sid,"RDY");
     DeleteFileA(path);
     bnspoolPath(path,dir,sid,"TXT");
     DeleteFileA(path);
}

/*--- helpers ------------------------------------------------------------*/

static VOID
putval(                            /* "name=value", value kept on one line */
FILE *fp,
const CHAR *name,
const CHAR *val)
{
     const CHAR *p;

     fprintf(fp,"%s=",name);
     for (p=val ; *p != '\0' ; p++) {
          fputc((*p == '\r' || *p == '\n') ? ' ' : *p,fp);
     }
     fputc('\n',fp);
}

/*
 * A spool id that sorts in arrival order: the time in seconds, then a
 * counter for articles stored within the same second.  Only the worker
 * thread creates spool files, so no locking is needed.
 */
static VOID
newsid(
const CHAR *dir,
CHAR *sid)
{
     static ULONG last=0;
     ULONG id=(ULONG)time(NULL);
     CHAR path[MAX_PATH];

     if (id <= last) {
          id=last+1;
     }
     for (;;) {
          sprintf(sid,"%08lX",id);
          bnspoolPath(path,dir,sid,"RDY");
          if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
               bnspoolPath(path,dir,sid,"TXT");
               if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
                    break;
               }
          }
          id++;
     }
     last=id;
}
