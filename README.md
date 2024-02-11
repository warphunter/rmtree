# rmtree

Rmtree can be used to remove a directory tree (recursive delete) in parallel.  Rmtree  is
       written  to be a fast, multi-threaded alternative to rm -rf, with some extended function‐
       ality.  While rm(1) is single-threaded, rmtree will by default use up to 8 CPU  cores  to
       remove  files and directories in parallel.  The basic idea is to handle each subdirectory
       as an independent unit, and feed a number of threads with these units.  Provided the  un‐
       derlying  storage system is fast enough, this scheme will speed up file removal consider‐
       ably.  Several options and flags can be used to remove files in a  directory  tree  in  a
       customized way.
