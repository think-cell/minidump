# mach-o minidump library

On Windows, `MiniDumpWriteDump` can produce dumps containing only the stack memory and thread registers. A crashing process can produce and send such minidumps to a backend server where they can be analyzed, manually and automatically, to find bugs that occur under specific conditions only. macOS had no such facility so we built it.

This repository contains the essential code for the error handling code in the crashing process, the out-of-process crash handler and the backend. 

The code does not compile. Some utility code to parse XML files, escape XML strings or read and write zip files is missing and should be replaced with your own implementation. We are also accepting pull requests of course. 

Depends on boost 1.74 and the [think-cell range library](https://github.com/think-cell/range)

## Client code

- Include the files in `writer/` in your code base
- `writer/DumpInfo.h` contains the `SDumpInfo` struct. The crashing process should call `SDumpInfo::Marshal` that sends all information to the crash handling process, e.g. through a pipe. The crash handler must call the `SDumpInfo` constructor.
- `writer/Minidump.cpp` should run in the crash handling process

## Backend setup

1. Build a cache of macOS system binaries
    - Setup VMs of supported macOS versions
    - Build [`dyld_shared_cache_util`](https://opensource.apple.com/source/dyld/dyld-519.2.1/launch-cache/dyld_shared_cache_util.cpp.auto.html)
    - Use `scripts/CopyMacOSSystemLibraries.py` to copy system libraries to server cache
2. Build a binary cache
    - Run `scripts/RebuildUuidDatabase.py` to index all binaries so you can look them up per uuid
    - Check the script for setup instructions

## Backend code

- `opendump.cpp` is the lldb command line driver that lets you open minidumps interactively in the shell
- Configure the path to the uuid index created by `RebuildUuidDatabase.py` in `opendump.cpp`
- In `LoadDump.cpp`, you need to configure where to find files describing your own debug symbols, how to mount the source code via http, and where to cache the system binaries locally.