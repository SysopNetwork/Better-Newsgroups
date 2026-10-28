/***************************************************************************
 *                                                                         *
 *   SNBETNWS.H                                                            *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   Message numbers for SNBETNWS.MSG.                                     *
 *                                                                         *
 *   Code finds a message by its position in the .MSG file.  The file     *
 *   starts with its only LEVEL6 marker, which takes no number, so the     *
 *   first message is 1.  New messages must be APPENDED at the end of the  *
 *   .MSG: inserting one renumbers every message after it.                 *
 *                                                                         *
 ***************************************************************************/

#ifndef SNBETNWS_H
#define SNBETNWS_H

#define SYSONLY      1             /* "this area is for the Sysop only"    */
#define MAINHDR      2             /* settings screen header               */
#define SETROW       3             /* one settings row                     */
#define MAINFTR      4             /* settings screen commands             */
#define ASKVAL       5             /* prompt for a new value               */
#define SAVED        6             /* setting saved                        */
#define BADVAL       7             /* value rejected                       */
#define BADSEL       8             /* unknown menu choice                  */
#define LEAVE        9             /* leaving the module                   */
#define PROMPT      10             /* settings menu prompt                 */
#define CHOICES     11             /* a list setting: its current value    */
#define CHOICEP     12             /* a list setting: the prompt           */
#define CHROW       13             /* one choice in a setting's list       */
#define GRPHDR      14             /* newsgroup list header                */
#define GRPROW      15             /* one newsgroup in the list            */
#define GRPNONE     16             /* no newsgroups set up yet             */
#define GRPFTR      17             /* newsgroup list commands and prompt   */
#define GASKNAME    18             /* add a newsgroup: its name            */
#define GASKFOR     19             /* add a newsgroup: its forum           */
#define GADDED      20             /* newsgroup added                      */
#define GBAD        21             /* newsgroup change refused             */
#define GEDHDR      22             /* one newsgroup's settings: header     */
#define GEDFTR      23             /* one newsgroup's settings: commands   */
#define GDELASK     24             /* confirm removing a newsgroup         */
#define GDELETED    25             /* newsgroup removed                    */
#define CHECKOK     26             /* "check started"                      */
#define CHECKBAD    27             /* check could not be started           */
#define QHDR        28             /* outgoing posts list header           */
#define QROW        29             /* one outgoing post                    */
#define QEMPTY      30             /* nothing waiting to be posted         */
#define QMORE       31             /* "...and more" after the list         */
#define HELPMENU    32             /* quick-start help (H at the menu)     */
#define HELPGRP     33             /* help on the newsgroup screens        */
#define SNDLINE     34             /* sender lines atop an imported article*/
#define PARTLINE    35             /* "(part N of M)" atop a long article  */
#define OVERSZ      36             /* text used for a too-large article    */
#define ATTNOTE     37             /* "attachments not imported" note      */
#define TAGLINE     38             /* signature added to outgoing posts    */
#define REJECT      39             /* notice to a user whose post failed   */
                                   /* help for each setting (? at its      */
                                   /* prompt), in settings order:          */
#define HENABLE     40             /*    1 Gateway enabled                 */
#define HHOST       41             /*    2 News server                     */
#define HPORT       42             /*    3 News server port                */
#define HSEC        43             /*    4 Connection security             */
#define HVERIFY     44             /*    5 Verify TLS certificate          */
#define HUSER       45             /*    6 Login name                      */
#define HPASS       46             /*    7 Password                        */
#define HPOLL       47             /*    8 Check for new articles          */
#define HTMOUT      48             /*    9 Timeout                         */
#define HBACK       49             /*   10 Articles for a new group        */
#define HMAXPER     50             /*   11 Most articles per check         */
#define HMAXKB      51             /*   12 Largest article                 */
#define HFNAME      52             /*   13 From name                       */
#define HDOMAIN     53             /*   14 Message-ID host name            */
#define HORG        54             /*   15 Organization                    */
#define HPOSTKEY    55             /*   16 Key needed to post              */
#define HRETRY      56             /*   17 Retry interval                  */
#define HGIVEUP     57             /*   18 Give up after                   */
#define HSHOWHDR    58             /*   19 Sender line in messages         */
#define HWRAP       59             /*   20 Word-wrap column                */
#define HKEEP       60             /*   21 Keep Message-IDs                */
#define HAUDOK      61             /*   22 Audit normal activity           */
#define HAUDBAD     62             /*   23 Audit errors                    */
#define HTRANS      63             /*   24 Transcript log file             */
                                   /* help for each newsgroup setting:     */
#define HGNAME      64             /*    1 Newsgroup                       */
#define HGFORUM     65             /*    2 Forum                           */
#define HGIMP       66             /*    3 Import articles                 */
#define HGEXP       67             /*    4 Export messages                 */
#define HGNEXT      68             /*    5 Next article number             */

#endif /* SNBETNWS_H */
