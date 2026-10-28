/***************************************************************************
 *                                                                         *
 *   BNVER.H                                                               *
 *                                                                         *
 *   Better Newsgroups (SNBETNWS) for The Major BBS v10                    *
 *   Copyright (C) 2026 Sysop Network.                                     *
 *   Released under the MIT License.                                       *
 *                                                                         *
 *   The single source of the version number.  Used by the C code (Audit  *
 *   Trail at startup, sysop screens, transcript log, User-Agent header)   *
 *   and by SNBETNWS.RC (the DLL's Windows version resource).              *
 *                                                                         *
 *   A release is shipped as DIST\v<version>, e.g. DIST\v1.0.0.            *
 *   The last FileVersion number counts every build and is never reused,  *
 *   so each binary has its own number: bump it for every build that      *
 *   leaves this machine.                                                  *
 *                                                                         *
 ***************************************************************************/

#ifndef BNVER_H
#define BNVER_H

#define BNVER_STR       "1.0.0"             /* shown to sysops             */
#define BNVER_DOTTED    "1.0.0.4"           /* Windows FileVersion string  */
#define BNVER_NUMS      1,0,0,4             /* Windows FILEVERSION numbers */

#define BNPRODUCT       "Better Newsgroups"
#define BNMODNAME       "SNBETNWS"
#define BNCOPYRIGHT     "Copyright (C) 2026 Sysop Network"

#endif /* BNVER_H */
