/***************************************************************************
 *                                                                         *
 *   BNSPOOL.H                                                             *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   The incoming spool folder (SNBETNWS\IN\ under the BBS directory)     *
 *   between the worker thread, which downloads and decodes articles, and  *
 *   the main thread, which imports them into the forums.  Each article   *
 *   is a pair of files named by an 8-digit hex spool id:                  *
 *                                                                         *
 *     xxxxxxxx.TXT   the text the user reads (CP437, '\n' line ends)      *
 *     xxxxxxxx.RDY   group, article number, Message-ID, references,      *
 *                    sender, subject, date (key=value text).  Written     *
 *                    LAST, by renaming, so a .RDY file always means the   *
 *                    pair is complete.                                    *
 *                                                                         *
 *   No BBS SDK calls: used by both threads and by the test programs.      *
 *                                                                         *
 ***************************************************************************/

#ifndef BNSPOOL_H
#define BNSPOOL_H

#include "BNNNTP.H"                /* first: winsock2 before windows.h */
#include "BNART.H"

#ifdef __cplusplus
extern "C" {
#endif

#define BNSP_IDSZ       12         /* a spool id ("0001A2B3") + room       */
#define BNSP_GROUPSZ    96         /* a newsgroup name                     */

typedef struct bnrdy {             /* one article waiting to be imported   */
     CHAR sid[BNSP_IDSZ];          /*   spool id (file names)              */
     CHAR group[BNSP_GROUPSZ];     /*   newsgroup it was read from         */
     BNARTNUM artnum;              /*   its number in that group           */
     CHAR msgid[BN_MIDSZ];         /*   Message-ID, without < >            */
     CHAR root[BN_MIDSZ];          /*   first reference ("" = starts one)  */
     CHAR parents[BN_MAXPAR][BN_MIDSZ]; /* parents, direct parent first    */
     INT npar;
     CHAR from[256];               /*   sender's address                   */
     CHAR fromname[128];           /*   sender's name, CP437               */
     CHAR replyto[256];            /*   Reply-To address, if any           */
     CHAR subject[256];            /*   subject, CP437                     */
     CHAR date[64];                /*   Date: as sent                      */
     LONG size;                    /*   article size in bytes              */
     BOOL oversize;                /*   too big: only the header was kept  */
     INT natt;                     /*   attached files (not imported)      */
} BNRDY;

VOID
bnspoolPath(                       /* full path of one spool file          */
CHAR *dst,                         /*   receives it (MAX_PATH)             */
const CHAR *dir,                   /*   spool folder, ending in '\'        */
const CHAR *sid,                   /*   spool id                           */
const CHAR *ext);                  /*   "TXT" or "RDY"                     */

BOOL                               /*   TRUE if both files were written    */
bnspoolWrite(                      /* store one decoded article            */
const CHAR *dir,                   /*   spool folder, ending in '\'        */
BNRDY *r,                          /*   details (sid is filled in here)    */
const CHAR *text,                  /*   CP437 text, '\n' line ends         */
size_t textlen);

BOOL                               /*   TRUE if read                       */
bnspoolRead(                       /* load one .RDY file                   */
const CHAR *path,                  /*   full path of the .RDY              */
BNRDY *r);

VOID
bnspoolDelete(                     /* remove one article's spool files     */
const CHAR *dir,
const CHAR *sid);

#ifdef __cplusplus
}
#endif

#endif /* BNSPOOL_H */
