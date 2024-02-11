# rmtree

Rmtree can be used to remove a directory tree (recursive delete) in parallel.  Rmtree  is written  to be a fast, multi-threaded alternative to rm -rf, with some extended functionality.  While rm(1) is single-threaded, rmtree will by default use up to 8 CPU  cores to remove files and directories in parallel.  The basic idea is to handle each subdirectory as an independent unit, and feed a number of threads with these units.  Provided the  underlying  storage system is fast enough, this scheme will speed up file removal considerably.  Several options and flags can be used to remove files in a  directory  tree  in  a customized way.

Rmtree consists of a 900 line re-usable library (commonlib.h) of functions written in C, in addition to the 800 lines specific part in rmtree.c. Rmtree can be compiled for almost any Unix/Linux out of the box, but should be easy to tailor to Unix versions I don't have access to. Building it for Windows requires a little more, and the necessary source code is located in the "win" subdirectory.

To build it for Unix/Linux, you just need gcc(1) or clang(1), and make(1). Default compiler in the Makefile is gcc, but you may switch to clang instead. Note that in my experience, gcc produces the fastest code. Just try running "make". If your Unix version isn't directly supported, you may try compiling it manually running "gcc -O2 rmtree.c -o rmtree -l pthread".

To build it for Windows (when you are using a Linux machine), run "make win". You need to have mingw-w64, mingw-w64-common and mingw-w64-x86-64-dev installed to be able to compile for 64-bit Windows, and additionally mingw-w64-i686-dev to compile for 32-bit. Rmtree can also be directly compiled on Windows using Cygwin.

You may run "make test" to perform a simple dry-run, or "make realtest" to create and delete 4 small trees in the same subdirectory as the source.

You may run "make install" to copy the binary to /usr/local/bin and the man page to /usr/local/share/man/man1 or to /usr/local/man/man1 if the first folder doesn't exist.

In the manual page rmtree.1 (or in rmtree.man which is preformatted), you will eventually find lots of examples and speed comparisons with rm(1).

Rmtree is tested and supported on Linux, FreeBSD, OpenBSD, MacOS, AIX, HP-UX, Solaris, Windows. You can use MinGW on Linux to compile it for Windows, or Cygwin directly on Windows. You can even compile it on a matching FreeBSD release and run it under the hood (in the system shell) of a NetApp cDOT node. To find the right FreeBSD version, you can run "file /bin/cat" in the NetApp system shell. After compilation and copying the resulting FreeBSD rmtree binary to the /var/home/diag/bin folder (which are already included in the PATH) on your NetApp node, you are ready to delete one or more directory trees in the various virtual file servers (SVMs) under the /clus mount point.
