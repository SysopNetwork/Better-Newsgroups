# Building Better Newsgroups

## Requirements

- Visual Studio 2022 or the 2022 Build Tools, C++ workload, toolset v143
- MBBS v10 module SDK (https://github.com/TheMajorBBS/MBBS-v10-module-SDK)
- Windows SDK 10

## Module DLL

1. Copy `SRC\Local.props.example` to `SRC\Local.props` and set `MBBS10SDK` to your SDK folder (with the trailing backslash).
2. Build Win32 (x86) only:

```
msbuild SRC\SNBETNWS.vcxproj /p:Configuration=Release /p:Platform=Win32
```

The output is `BUILD\Release\SNBETNWS.dll`, with copies of the `.MSG`, `.MDF` and `.RLN`. The runtime is linked statically (`/MT`), so the BBS server needs no Visual C++ runtime. `/PDBALTPATH` keeps the build path out of the DLL.

Check a build with `dumpbin /exports` (must show `_init__snbetnws`) and `dumpbin /headers` (machine 14C).

## Tests and tools

```
TESTS\RUNTESTS.CMD               unit tests (exit code 0 = pass); no BBS, no network
TOOLS\BUILDTOOLS.CMD             builds BUILD\TOOLS\BNFETCH.EXE
BUILD\TOOLS\BNFETCH.EXE test     log in to the server in BNFETCH.INI, open each group
BUILD\TOOLS\BNFETCH.EXE fetch alt.test 5    read and decode the last 5 articles
BUILD\TOOLS\BNFETCH.EXE session 3           one full worker session, backfill 3
BUILD\TOOLS\BNFETCH.EXE build               print a sample outgoing article
BUILD\TOOLS\BNFETCH.EXE post alt.test YES   post the sample article (publishes!)
```

Copy `TOOLS\BNFETCH.INI.EXAMPLE` to `TOOLS\BNFETCH.INI` (never committed) and fill it in. `-v` shows the server conversation.

## Version and release

- `SRC\BNVER.H` is the only place the version lives. Bump the last FILEVERSION number for every build that leaves the development machine, and never reuse it.
- A release goes in `DIST\v<version>\`: `SNBETNWS.DLL`, `.MDF`, `.MSG`, `.RLN`, nothing else, uppercase names.
- Deploying the `.MSG` replaces any text the Sysop edited in it; say so in the release notes when it changed.
- Before a release: update `CHANGELOG.MD` and `README.MD`, run the tests, and check that no credentials, hostnames or local paths are in the repository.
