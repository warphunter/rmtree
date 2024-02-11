/*
   rmtree - fast parallel file tree deletion

   Copyright (C) 2020 - 2024 by Jorn I. Viken <jornv@1337.no>

   Portions of this code are derived from software created by
   - Dmitry Yu Okunev <dyokunev@ut.mephi.ru>, https://github.com/xaionaro/libpftw (pthreads framework)
   - Martin Kunev <martinkunev@gmail.com>, https://gist.github.com/martinkunev/1365481 (heap code)
   - Shadkam I. <>, https://github.com/shadkam/recentmost (recentmost)

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define VERSION "1.25"
#define RMTREE

#define _GNU_SOURCE

#include <errno.h>
#include <semaphore.h>
#if defined(__APPLE__)
#   include <dispatch/dispatch.h>
#endif
#include <signal.h>
#if defined(_AIX)
#    define _SIGSET_T	// needed at least on GCC 4.8.5 to avoid these msgs: "error: conflicting types for 'sigset_t'"
#endif
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <regex.h>
#include <stdlib.h>
#include <search.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#if ! defined(__MINGW32__)
#    include <pwd.h>
#    include <grp.h>
#endif

#if defined(__hpux)
#   include <sys/pstat.h>
#endif

#if defined (__sun__)
#    include <sys/statvfs.h>
#elif defined(__hpux)
#    include <sys/vfs.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
#    include <sys/param.h>
#    include <sys/mount.h>
#elif defined(__MINGW32__)
#    include "os-hacks.h"
#    define LINE_MAX 2048
#    define __USE_MINGW_ANSI_STDIO 1
#    define _POSIX_SEM_VALUE_MAX 32767
#    define lstat(x,y) stat(x,y)
#endif

#undef FALSE
#undef TRUE
typedef enum {FALSE, TRUE} boolean;

#define MAYBE_UNUSED     __attribute__ ((unused))

#if defined (__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#    include <fcntl.h>
#    include <sys/syscall.h>
#    define DEFAULT_DIRENT_COUNT 100000		// - for option -X, may be overridden using env var DIRENTS
    static boolean extreme_readdir = FALSE; 	// - set to TRUE if option -X is given
    static unsigned buf_size;			// - set if option -X is given
    static unsigned getdents_calls;		// - incremented for every syscall(SYS_getdents, ... (if option -X is given)
#endif

// Borrowed from /usr/include/nspr4/pratom.h on RH6.4:
#if ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)) && ! defined(__hppa__)
#    define PR_ATOMIC_ADD(ptr, val) __sync_add_and_fetch(ptr, val)
#else
     static pthread_mutex_t accum_filecnt_lock = PTHREAD_MUTEX_INITIALIZER; // for protecting "accum_filecnt"
#endif

#define ASCEND TRUE
#define DESCEND FALSE

#define INLINE_PROCESSING_THRESHOLD	2

#define MAX_THREADS        	512	// - max number of threads that may be created

#define DIRTY_CONSTANT		~0 	// - for handling non-POSIX compliant file systems
			   		// (link count should reflect the number of subdirectories, and should be 2 for empty directories)

static char *progname;
static boolean xdev = FALSE;		// - set to TRUE if option -x is given

static boolean master_finished = FALSE;

static unsigned statcount = 0; // - counter for lstat calls
MAYBE_UNUSED static pthread_mutex_t statcount_lock = PTHREAD_MUTEX_INITIALIZER;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
    unsigned statcount_unexp = 0;
    MAYBE_UNUSED static pthread_mutex_t statcount_unexp_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static unsigned queued_dirs;	// - the total number of dirs handled by a separate thread
MAYBE_UNUSED static pthread_mutex_t queued_dirs_lock = PTHREAD_MUTEX_INITIALIZER;

static boolean simulate_posix_compliance = FALSE; // - POSIX requires the directory link count to be at least 2

static unsigned long inline_processing_threshold = INLINE_PROCESSING_THRESHOLD;

static boolean lifo_queue = TRUE;       // - default queue of directories to be processed is of type LIFO
static boolean fifo_queue = FALSE;      // - select a standard FIFO queue with option -q
static boolean ino_queue = FALSE;       // - select a sorted queue of dirents with option -Q
static unsigned long inolist_bypasscount;// - total number of list elements bypassed; only used by inodirlist_bintreeinsert() 

static boolean debug = FALSE;		// - set if env var DEBUG is set

enum filetype {
	LIST_REGFILE=1,
	LIST_DIR=2,
	LIST_SYMLINK=4,
	LIST_BLOCKDEV=8,
	LIST_CHARDEV=16,
	LIST_PIPE=32,
	LIST_SOCKET=64
};

static unsigned accum_filecnt = 0;	  // - count the number of files seen
static unsigned verbose_count = 0;	  // - set if option -v is specified
static unsigned last_accum_filecnt = 0;   // - used by -v
static pthread_mutex_t last_accum_filecnt_lock = PTHREAD_MUTEX_INITIALIZER; // for protecting "accum_filecnt"
static unsigned just_count = 0; 	  // - never used in rmtree
static time_t last_t = 0;                 // - previous timestamp in seconds since EPOCH, used in pthread_routine()

static char **excludelist;		  // - set if -e/-E is specified
static unsigned excludelist_count = 0;	  // - set if -e/-E is specified
static regex_t **excluderecomp;		  // - set if -e/-E is specified

boolean dryrun = FALSE; // - set to TRUE if -n is specified; don't delete anything; just print files/dirs to be deleted

static unsigned entries_deleted = 0;	  // - total number of files/dirs deleted.
MAYBE_UNUSED static pthread_mutex_t entries_deleted_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned dir_not_empty = 0;        // - counter for unsuccessful rmdir() calls, type EEXIST
static unsigned dir_already_deleted = 0;  // - counter for unsuccessful rmdir() calls, type ENOENT
static unsigned dir_other_reason = 0;     // - counter for unsuccessful rmdir() calls, type "any other reason"

static boolean final_pass = FALSE; 	  // Used to signal final rmdir only pass.
					  // This is a sequential operation on the remaining directory structure.
static unsigned final_pass_cnt = 0;	  // Counter for number of dirs deleted in final pass

static pthread_mutex_t perror_lock = PTHREAD_MUTEX_INITIALIZER; // The perror() function should be allowed to finish printing.

typedef struct dirlist dirlist_t;

struct dirlist {
	char		*dirpath;
	unsigned	 depth;		  // - Current directory depth.
	unsigned	 inlined;  	  // - How many subdirs are processed inline so far.
	boolean		 all_inlined;  	  // - Used to flag that one or more subdirs below are not inline handled.
        unsigned         filecnt;         // - sum of files in this dir and inline processed subdirs
	dirlist_t	*next;	    	  // - A pointer to next directory in queue.
        dirlist_t       *prev;            // - pointer to previous directory in queue
	unsigned	 st_nlink;	  // - Link count for current directory = number of subdirs incl "." and "..".
	unsigned long	 st_dev;	  // - File system id for current directory.
        ino_t            st_ino;          // - directory inode number
};

// This is the global list of directories to be processed, malloc'ed later:
dirlist_t	*dirlist_head;	    // - first directory in queue
dirlist_t	*dirlist_tail;	    // - last directory in queue - only for FIFO queue (option -Q)
unsigned	 queuesize = 0;	    // - current number of queued directories waiting to be processed by a thread
unsigned	 maxdepth = 0;	    // - max directory depth, if option -m is specified
pthread_mutex_t	 dirlist_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting dirlist_head, dirlist_tail, queuesize

static pthread_t	*thread_arr	 	= NULL;
static unsigned		 thread_cnt	 	= 0; // - set by main(), used by traverse_trees(), thread_prepare(), thread_cleanup()
static unsigned		 sleeping_thread_cnt	= 0;
#if ! defined(PR_ATOMIC_ADD)
	static pthread_mutex_t   sleeping_thread_cnt_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting "sleeping_thread_cnt"
#endif

#if ! defined(__APPLE__)
	static sem_t	 master_sem;
	static sem_t	 threads_sem;
	static sem_t	 finished_threads_sem;
#else
	static dispatch_semaphore_t
			 master_sem;
	static dispatch_semaphore_t
			 threads_sem;
	static dispatch_semaphore_t
			 finished_threads_sem;
#endif
static unsigned		 sem_val_max_exceeded_cnt = 0;	// - _POSIX_SEM_VALUE_MAX == 32767, so this variable counts all above this value
static pthread_mutex_t	 sem_val_max_exceeded_cnt_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting "sem_val_max_exceeded_cnt"

/////////////////////////////////////////////////////////////////////////////

static inline void do_rmdir(
	const char *path)
{
	int rc;

        rc = rmdir(path);
        if (rc < 0) {
                switch (errno) {
                	case ENOTEMPTY:
                        	dir_not_empty++;
                                break;
                        case ENOENT:
                                dir_already_deleted++;
                                break;
                        default:
                                dir_other_reason++;
                }
		pthread_mutex_lock(&perror_lock);
                perror("rmdir()");
		pthread_mutex_unlock(&perror_lock);
	} else {
#             if defined(PR_ATOMIC_ADD)
                PR_ATOMIC_ADD(&entries_deleted, 1);
#             else
                pthread_mutex_lock(&entries_deleted_lock);
		entries_deleted++;
                pthread_mutex_unlock(&entries_deleted_lock);
#             endif
	}
}

/////////////////////////////////////////////////////////////////////////////

#include "commonlib.h"

/////////////////////////////////////////////////////////////////////////////

// Used by walk_dir:
static inline void handle_dirent(dirlist_t *, struct dirent *);

/////////////////////////////////////////////////////////////////////////////

static void walk_dir(
	dirlist_t *curdir)
{
	DIR *dir = NULL;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	unsigned fd = 0;    		// - only used on Linux/*BSD if option -X is given
	char *buf = NULL;   		// - same
	unsigned bpos = 0, nread = 0;	// - same
#endif
	struct dirent *dent = NULL;

#     if defined(DEBUG2)
	if (getenv("DEBUG2") && curdir->depth <= 2)
		fprintf(stderr, "- opendir(%s)\n", curdir->dirpath);
#     endif
	//assert(curdir->dirpath);

#    if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	if (extreme_readdir) {
		if ((fd = open(curdir->dirpath, O_RDONLY | O_DIRECTORY)) < 0) {
			pthread_mutex_lock(&perror_lock);
			perror(curdir->dirpath);
			pthread_mutex_unlock(&perror_lock);
			return;
		}
		dent = malloc(sizeof(struct dirent));
		assert(dent);
		buf = malloc(buf_size);
		assert(buf);
	} else
#    endif
	if (! (dir = opendir(curdir->dirpath))) {
			pthread_mutex_lock(&perror_lock);
			perror(curdir->dirpath);
			pthread_mutex_unlock(&perror_lock);
			return;
	}

	if (curdir->st_nlink < 2 && ! simulate_posix_compliance && ! final_pass) {
		if (debug)	
			fprintf(stderr, "POSIX non-compliance detected on %s - setting simulate_posix_compliance = TRUE\n", curdir->dirpath);
		simulate_posix_compliance = TRUE;
		curdir->st_nlink = DIRTY_CONSTANT;
	}

	while (TRUE) {
		//assert(dir); // - something is seriously wrong if dir == 0 here...
#	      if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
		if (extreme_readdir) {
			readdir_extreme(fd, buf, buf_size, curdir->dirpath, &bpos, dent, &nread);
			if (! nread) {
				close(fd);
				free(buf);
				free(dent);
				dent = NULL;
			}
		} else
#	      endif
		dent = readdir(dir);

		if (dent == NULL)
			break;

#	     if defined(DEBUG2)
		if (getenv("DEBUG2") && curdir->depth <= 2)
			fprintf(stderr, "- readdir(%s) done, dent=<%s>\n", curdir->dirpath, dent->d_name);
#	     endif

		if (dent->d_name[0] == '.' && 
			(dent->d_name[1] == 0 ||
			(dent->d_name[1] == '.' && dent->d_name[2] == 0)))
				continue;       // Skip "." and ".."

		handle_dirent(curdir, dent);
	}

#     if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	if (! extreme_readdir)
#     endif
		closedir(dir);

	if (! dryrun &&
	    ! final_pass &&
	      curdir->all_inlined &&
	      (curdir->st_nlink < 3 + inline_processing_threshold		// - no more subdirs left.
		|| (! curdir->inlined && inline_processing_threshold)) &&	// - had no subdirs.
		! (curdir->dirpath[0] == '.' && curdir->dirpath[1] == '\0'))
		do_rmdir(curdir->dirpath);

	if (curdir->dirpath)
		free(curdir->dirpath);

	return;
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) void handle_dirent(
	dirlist_t *curdir,
	struct dirent *dent)
{
	boolean dive_into_subdir = FALSE;
	int rc, i;
	struct stat st;
	st.st_dev = 0;

	// Getting path
	size_t path_len = strlen(curdir->dirpath) + 1 + strlen(dent->d_name);
	char *path = malloc(path_len+1);
	assert(path);
	strcpy(path, curdir->dirpath);
	if (! (path[0] == '/' && path[1] == 0)) strcat(path, "/"); // - only add / if path != /
	strcat(path, dent->d_name);

	// Running stat if there might be subdirs below...
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
	if ((dent->d_type == DT_DIR || (dent->d_type == DT_UNKNOWN && curdir->st_nlink > 2))
	    && ! final_pass) {
		// We might get d_type == DT_UNKNOWN (0):
		// - on directories we don't own ourselves.
		// - on NFS shares.
		rc = lstat(path, &st);
		if (rc && errno == EACCES) {
			pthread_mutex_lock(&perror_lock);
			perror(path);
			pthread_mutex_unlock(&perror_lock);
		}

		if (dent->d_type == DT_UNKNOWN) {
#                     if defined(PR_ATOMIC_ADD)
                        PR_ATOMIC_ADD(&statcount_unexp, 1);
#                     else
                        pthread_mutex_lock(&statcount_unexp_lock);
			statcount_unexp++;
                        pthread_mutex_unlock(&statcount_unexp_lock);
#                     endif

			switch (st.st_mode & S_IFMT) {
				case S_IFREG:
					dent->d_type = DT_REG;
					break;
				case S_IFDIR:
					dent->d_type = DT_DIR;
					break;
#			if ! defined(__MINGW32__)
				case S_IFBLK:
					dent->d_type = DT_BLK;
					break;
				case S_IFCHR:
					dent->d_type = DT_CHR;
					break;
				case S_IFIFO:
					dent->d_type = DT_FIFO;
					break;
				case S_IFLNK:
					dent->d_type = DT_LNK;
					break;
				case S_IFSOCK:
					dent->d_type = DT_SOCK;
					break;
#			endif
			}
		} else {
#     		      if defined(PR_ATOMIC_ADD)
        		PR_ATOMIC_ADD(&statcount, 1);
#     		      else
        		pthread_mutex_lock(&statcount_lock);
			statcount++;
        		pthread_mutex_unlock(&statcount_lock);
#     		      endif
		}
	}

	if ((curdir->st_nlink > 2 && dent->d_type == DT_DIR) || final_pass) {
		dive_into_subdir = TRUE;

		if (xdev && curdir->st_dev != st.st_dev)
			dive_into_subdir = FALSE;
	}
#else // - non-Linux/BSD goes here: // - non-Linux/BSD goes here:
	if (curdir->st_nlink > 2) {
#             if defined(PR_ATOMIC_ADD)
                PR_ATOMIC_ADD(&statcount, 1);
#             else
                pthread_mutex_lock(&statcount_lock);
                statcount++;
                pthread_mutex_unlock(&statcount_lock);
#             endif

		rc = lstat(path, &st);
		if (rc && errno == EACCES) {
			pthread_mutex_lock(&perror_lock);
			perror(path);
			pthread_mutex_unlock(&perror_lock);
		}

		if (S_ISDIR(st.st_mode)) {
			dive_into_subdir = TRUE;

			if (xdev && curdir->st_dev != st.st_dev)
				dive_into_subdir = FALSE;
		}
	}
#endif

	if (dive_into_subdir) {
		if (maxdepth) {
			if (curdir->depth >= maxdepth) {
				free(path);
				return;
			}
		}
		if (excludelist_count) {
                for (i = 0; i < excludelist_count; i++)
                        if (excluderecomp) {
                                if (regexec(excluderecomp[i], dent->d_name, 0, NULL, 0) == 0) {
                                        if (debug) fprintf(stderr, "==> Skipping dir %s (%s)\n", path, excludelist[i]);
                                        return;         // - skip directories specified through -e
                                }
                        } else {
                                if (strcmp(excludelist[i], dent->d_name) == 0) {
                                        if (debug) fprintf(stderr, "==> Skipping dir %s (%s)\n", path, excludelist[i]);
                                        return;         // - skip directories specified through -E
                                }
                        }
        	}

		if (dryrun)
			puts(path);

		if (curdir->inlined >= inline_processing_threshold && ! final_pass) {
			// If there are more subdirs than inline_processing_threshold, then publish all the rest for the threads
			curdir->all_inlined = FALSE;
			dirlist_add_dir(path, curdir->depth+1, &st);
		} else {
			// Perform the subdirentry inline as long as we haven't reached inline_processing_threshold.
			curdir->inlined++;

			dirlist_t subdirentry;

			subdirentry.dirpath = strdup(path);
			assert(subdirentry.dirpath);
			subdirentry.all_inlined = curdir->all_inlined;
			subdirentry.depth = curdir->depth+1;
			subdirentry.inlined = 0;
			subdirentry.st_nlink = simulate_posix_compliance ? DIRTY_CONSTANT : st.st_nlink;
			subdirentry.st_dev = st.st_dev;
			subdirentry.filecnt = 0;

			walk_dir(&subdirentry);
			curdir->all_inlined = subdirentry.all_inlined;

			if (final_pass) {
				// Invariant: subdir is empty and can be deleted
				do_rmdir(path);
				final_pass_cnt++;
			}
		}
	} else {
		// assert(dent->d_type != DT_DIR); // - just for the BSD/Linux family
                if (dryrun) {
                        puts(path);
                } else {
                        rc = unlink(path);
                        if (rc < 0)
                                perror(path);
			else {
#             		      if defined(PR_ATOMIC_ADD)
                		PR_ATOMIC_ADD(&entries_deleted, 1);
#             		      else
                		pthread_mutex_lock(&entries_deleted_lock);
                		entries_deleted++;
                		pthread_mutex_unlock(&entries_deleted_lock);
#             		      endif
			}
		}
	}

	free(path);

	return;
}

/////////////////////////////////////////////////////////////////////////////

static int usage()
{
        printf("Usage: %s [-t <count>] [-e <dir> ... | -E <dir> ... | -Z]\n", progname);
	printf("\t      [-m <maxdepth>] [-x] [-I <count>] [-q] [-X] [-S] [-T] [-V] [-h] [-d] arg1 [arg2 ...]\n");
        printf("-t <count>\t Run up to <count> threads in parallel.\n");
        printf("\t\t * Must be a non-negative integer between 1 and %i.\n", MAX_THREADS);
        printf("\t\t * Defaults to (virtual) CPU count on host, up to 8.\n");
        printf("\t\t * Note that <count> threads will be created in addition to the main thread, so the total number of\n");
        printf("\t\t   threads will be <count+1>, but the main thread won't do any hard work, and will be mostly idle.\n\n");

        printf("-e <dir>\t Exclude directory matching <dir> from traversal.\n");
        printf("\t\t * Extended regular expressions are supported.\n");
        printf("\t\t * Any number of -e options are supported, up to command line limit.\n\n");

	printf("-E <dir>\t Exclude directory <dir> from traversal.\n");
        printf("\t\t * For simplicity, only exact matches are excluded.\n");
        printf("\t\t * Any number of -E options are supported, up to command line limit.\n");
        printf("\t\t * Hint: Excluding .snapshot is usually desired on (the root of) NFS shares from NAS.\n\n");

#if ! defined(__MINGW32__)
        printf("-Z\t\t Equivalent to -E.snapshot.\n");
        printf("\t\t * Just to save some typing since it is commonly needed on a NAS NFS share.\n");
        printf("\t\t * Not implemented for Windows.\n\n");
#endif

        printf("-m <maxdepth>\t Descend at most <maxdepth> (a positive integer) levels below the start point(s).\n");
        printf("\t\t * This equals the -maxdepth option to GNU find(1).\n\n");

        printf("-x\t\t Only search the file system containing the directory specified.\n");
        printf("\t\t * This equals the -xdev option to find(1).\n\n");

	printf("-q\t\t Organize the queue of directories as a FIFO which may be faster in some cases (default is LIFO).\n");
        printf("\t\t * The speed difference between a LIFO and a FIFO queue is usually small.\n");
        printf("\t\t * Note that this option will use more memory.\n\n");

        printf("-Q\t\t Organize the queue of directories as a list sorted on inode number.\n");
        printf("\t\t * Using this option with a file system on a single (or mirrored) spinning disk is recommended.\n");
        printf("\t\t * Using it on a storage array or on SSD or FLASH disk is probably pointless.\n\n");

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
        printf("-X\t\t May be used to speed up %s'ing eXtremely big directories containing millions of files.\n", progname);
        printf("\t\t * This option is probably only useful when the big directories being traversed are cached in memory.\n");
        printf("\t\t * With this option, default maximum number of dirents read in one go is 100000.\n");
        printf("\t\t * Environment variable DIRENTS may be set to override the default.\n");
        printf("\t\t * This option is only supported on Linux and *BSD flavors.\n\n");
#endif
        printf("-I <count>\t Use <count> as number of subdirectories in a directory, that should\n");
        printf("\t\t be processed in-line instead of processing them in separate threads.\n");
        printf("\t\t * Default is to process up to two subdirectories in a directory in-line.\n");
        printf("\t\t * If there are no more than <count> subdirectories, all will be processed in-line.\n");
        printf("\t\t * If there are more than <count> subdirectories, say n, the first n - <count>\n");
        printf("\t\t   will be enqueued to avoid thread starvation.\n");
        printf("\t\t * This is a performance option to possibly squeeze out even faster run-times.\n");
        printf("\t\t * Use 0 for processing every subdirectory in a separate thread, and no in-line processing.\n\n");


        printf("-d\t\t Can be used to dry-run before actually deleting anything.\n");
        printf("\t\t * Files and directories will just be listed on stdout, and WILL NOT be deleted.\n\n");

        printf("-S\t\t Print some stats to stderr when finished.\n");
        printf("-T\t\t Print the elapsed real time between invocation and termination of the program on stderr, like time(1).\n");
        printf("-V\t\t Print out version and exit.\n");
        printf("-h\t\t Print this help text.\n");

        printf("\n* If no argument is specified, this help text will be printed to stdout.\n");
        printf("* All arguments (arg1 arg2 ...) should be directories or symlinks to directories.\n");
        printf("  If some of them are not, they will just be excluded and an error message will be printed for each.\n");
        printf("  All files and directories below the start point(s) will by default be deleted in parallel (in addition to the start point(s)).\n");
        printf("* The program has been tested with start point(s) on these file systems:\n");
        printf("  - Linux: ext2, ext3, ext4, xfs, jfs, btrfs, nilfs2, f2fs, zfs, tmpfs\n");
	printf("           reiserfs, hfs plus, minix, bfs, ntfs (fuseblk), vxfs, gpfs\n");
        printf("  - FreeBSD: ufs, zfs, devfs, ms-dos/fat\n");
        printf("  - OpenBSD: ffs\n");
        printf("  - NetApp systemshell (FreeBSD): clusfs\n");
        printf("  - MacOS: apfs\n");
        printf("  - AIX: jfs, jfs2, ahafs\n");
        printf("  - HP-UX: vxfs, hfs\n");
        printf("  - Solaris: zfs, ufs, udfs\n");
        printf("  - Windows (MinGW, Cygwin): ntfs\n");
        printf("  - All: nfs\n");

	printf("* The program contains code inspired by https://github.com/xaionaro/libpftw\n");
	printf("* The program makes use of heap algorithms derived from https://gist.github.com/martinkunev/1365481\n");
	printf("* The program makes use of https://github.com/coapp-packages/libgnurx when being built for Windows\n");
	printf("* Warning: This program may impose a very high load on your storage systems when utilizing many CPU cores.\n");
	printf("* The \"%s\" program comes with ABSOLUTELY NO WARRANTY.  This is free software, and you are welcome\n", progname);
	printf("  to redistribute it under certain conditions.  See the GNU General Public Licence for details.\n");

	printf("\nCopyright (C) 2020 - 2024 by Jorn I. Viken, jornv@1337.no.\n");
	return -1;
}

/////////////////////////////////////////////////////////////////////////////

int main(
	int argc,
	char *argv[])
{
	char **startdirs;
	unsigned startdircount;
	int ch;
	boolean stats = FALSE;
	boolean e_option = FALSE, E_option = FALSE;
	struct timeval starttime;
	boolean timer = FALSE;
	unsigned threads = 1;
#    if defined(__hpux)
	struct pst_dynamic psd;

	if (pstat_getdynamic(&psd, sizeof(psd), (size_t)1, 0))
		threads = (unsigned) psd.psd_proc_cnt;
#    elif defined(__MINGW32__)
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	threads = sysinfo.dwNumberOfProcessors;
#    else
	threads = (unsigned) sysconf(_SC_NPROCESSORS_ONLN);
#    endif
	if (threads > 8)
		threads = 8; // - using 8 as default max number of threads

        progname = strrchr(argv[0], '/');
        if (progname)
                progname++; // - move pointer past the found '/'
        else
                progname = argv[0];

	debug = getenv("DEBUG") != NULL;

	tzset(); // - core dumps on Ubuntu 16.04.6 LTS with kernel 4.4.0-174-generic when executed through localtime() at the end of main()

	while ((ch = getopt(argc, argv, "ht:I:e:E:Zm:dvxqQSTVX")) != -1)
		switch (ch) {
			case 't':
				threads = atoi(optarg);
				if (threads < 1 || threads > MAX_THREADS)
					return usage();
				break;
			case 'I':
				inline_processing_threshold = atoi(optarg);
				break;
			case 'e':
				if (E_option) {
					fprintf(stderr, "Option -e can not be combined with -E.\n");
					exit(1);
				}
				excludelist = realloc(excludelist, (excludelist_count + 1) * sizeof(excludelist));
				assert(excludelist);
				excludelist[excludelist_count] = optarg;
				excluderecomp = realloc(excluderecomp, (excludelist_count + 1) * sizeof(excluderecomp));
				assert(excluderecomp);
				if (! regex_init(&excluderecomp[excludelist_count], optarg, REG_EXTENDED|REG_NOSUB))
					return usage();
				excludelist_count++;
				e_option = TRUE;
				break;
			case 'E':
			case 'Z':
				if (e_option) {
					fprintf(stderr, "Option -E / -Z can not be combined with -e.\n");
					exit(1);
				}
				excludelist = realloc(excludelist, (excludelist_count + 1) * sizeof(excludelist));
				assert(excludelist);
                        	if (ch == 'E')
                                	excludelist[excludelist_count++] = optarg;
                        	else
                                	excludelist[excludelist_count++] = ".snapshot";
				E_option = TRUE;
				break;
			case 'm':
				if (atoi(optarg) < 1)
					return usage();
				maxdepth = atoi(optarg);
				break;
			case 'd':
				dryrun = TRUE;
				break;
			case 'v':
				if (atoi(optarg) < 1)
					return usage();
				verbose_count = atoi(optarg);
				break;
			case 'x':
				xdev = TRUE;
				break;
			case 'q':
				fifo_queue = TRUE;
				lifo_queue = FALSE;
				break;
			case 'Q':
				lifo_queue = FALSE;
				fifo_queue = FALSE;
				ino_queue = TRUE;
				break;

			case 'S':
				stats = TRUE;
				break;
			case 'T':
				timer = TRUE;
				(void) gettimeofday(&starttime, NULL);
				break;
			case 'V':
				printf("%s\n", VERSION);
				exit(0);
				break;
			case 'X':
#			      if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
				extreme_readdir = TRUE;
				if (getenv("DIRENTS"))
					buf_size = atoi(getenv("DIRENTS")) * sizeof(struct dirent);
				else
					buf_size = DEFAULT_DIRENT_COUNT * sizeof(struct dirent);
#			      else
				fprintf(stderr, "Option -X is not implemented for this OS.\n");
				exit(1);
#			      endif
				break;
			case 'h':
			case '?':
			default:
				return usage();
		}

	argc -= optind;
	argv += optind;

	if (argc == 0) {
		return usage();
	} else {
		startdirs = argv;
		startdircount = argc;
	}

	if (threads == 1)
                inline_processing_threshold = DIRTY_CONSTANT; // - process everything inline if we have just 1 CPU...

	thread_cnt = threads;
	thread_prepare();

	traverse_trees(startdirs, startdircount);

	thread_cleanup();

	if (timer) {
		struct timeval endtime;
		(void) gettimeofday(&endtime, NULL);
		fflush(stdout);
		fprintf(stderr, "Real: %.2f seconds\n",
			(double)(((endtime.tv_sec-starttime.tv_sec)*1000 + (endtime.tv_usec-starttime.tv_usec) / 1000)) / 1000);
	}

	if (stats) {
		fprintf(stderr, "+------------------------------+\n");
		fprintf(stderr, "| Some final tidbits from \"-S\" |\n");
		fprintf(stderr, "+------------------------------+\n");
		fprintf(stderr, "- Version: %s\n", VERSION);
		fprintf(stderr, "- Number of active threads used: %i\n", threads);
		fprintf(stderr, "- Max number of subdirectories that could be processed in-line per directory\n");
		fprintf(stderr, "  (and not in a separate thread): %lu\n", inline_processing_threshold);
#if 	      defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
		if (extreme_readdir) {
			fprintf(stderr, "- Number of SYS_getdents system calls = %u\n", getdents_calls);
			fprintf(stderr, "- Used DIRENTS = %lu\n", (unsigned long)buf_size / sizeof(struct dirent));
		}
#	      endif
		fprintf(stderr, "- Mandatory lstat calls (at least 1 per directory): %i\n", statcount);
#	      if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
		fprintf(stderr, "- Unexpected lstat calls (when returned d_type is DT_UNKNOWN): %i\n", statcount_unexp);
#	      endif
                fprintf(stderr, "- Number of %s enqueued directories: %i\n", fifo_queue ? "FIFO" : (ino_queue ? "INODE" : "LIFO"), queued_dirs);
		fprintf(stderr, "- Number of files/directories deleted: %i\n", entries_deleted);
		fprintf(stderr, "- Number of directories deleted in final pass: %i\n", final_pass_cnt);
                fprintf(stderr, "- Unsuccessful rmdir() calls, type EEXIST: %i\n", dir_not_empty);
                fprintf(stderr, "- Unsuccessful rmdir() calls, type ENOENT: %i\n", dir_already_deleted);
                fprintf(stderr, "- Unsuccessful rmdir() calls, type \"any other reason\": %i\n", dir_other_reason);

#             if defined(PR_ATOMIC_ADD)
		fprintf(stderr, "- Program compiled with support for __sync_add_and_fetch\n");
#             endif
#             if defined(CC_USED)
                fprintf(stderr, "- Compiled using: %s\n", CC_USED);
#             endif
	}
	return 0;
}
