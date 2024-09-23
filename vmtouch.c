/***********************************************************************
vmtouch - the Virtual Memory Toucher

Portable file system cache diagnostics and control

by Doug Hoyte (doug@hcsw.org)

Compilation:
  gcc -Wall -O3 -o vmtouch vmtouch.c

************************************************************************

Copyright 2009-2023 Doug Hoyte and contributors

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***********************************************************************/


#define VMTOUCH_VERSION "1.3.1"
#define RESIDENCY_CHART_WIDTH 60
#define CHART_UPDATE_INTERVAL 0.1
#define MAX_CRAWL_DEPTH 1024
#define MAX_NUMBER_OF_IGNORES 1024
#define MAX_NUMBER_OF_FILENAME_FILTERS 1024
#define MAX_FILENAME_LENGTH 1024

#if defined(__linux__) || (defined(__hpux) && !defined(__LP64__))
// Make sure off_t is 64 bits on linux and when creating 32bit programs
// on HP-UX.
#define _FILE_OFFSET_BITS 64
#endif

#ifdef __linux__
// Required for posix_fadvise() on some linux systems
#define _XOPEN_SOURCE 600
// Required for mincore() on some linux systems
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
// for O_NOATIME
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <limits.h>
#include <inttypes.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <search.h>
#include <libgen.h>
#include <fnmatch.h>

#if defined(__linux__)
// Used to find size of block devices
#include <sys/ioctl.h>
#include <sys/mount.h>
// Used to check kernal version to know if mincore reports correctly
#include <sys/utsname.h>
#endif

/*
 * To find out if the stat results from a single file correspond to a file we
 * have already seen, we need to compare both the device and the inode
 */
struct dev_and_inode
{
    dev_t dev;
    ino_t ino;
};

long pagesize;

int64_t total_pages=0;
int64_t total_pages_in_core=0;
int64_t total_files=0;
int64_t total_dirs=0;

int64_t offset=0;
int64_t max_len=0;

unsigned int junk_counter; // just to prevent any compiler optimizations

int curr_crawl_depth=0;
ino_t crawl_inodes[MAX_CRAWL_DEPTH];

// remember all inodes (for files with inode count > 1) to find duplicates
void *seen_inodes = NULL;
dev_t orig_device = 0;
int orig_device_inited = 0;


int o_touch=0;
int o_evict=0;
int o_quiet=0;
int o_verbose=0;
int o_lock=0;
int o_lockall=0;
int o_daemon=0;
int o_followsymlinks=0;
int o_singlefilesystem=0;
int o_ignorehardlinkeduplictes=0;
size_t o_max_file_size=SIZE_MAX;
int o_wait=0;
static char *o_batch = NULL;
static char *o_pidfile = NULL;
static char *o_output = NULL;
int o_0_delim = 0;


char *ignore_list[MAX_NUMBER_OF_IGNORES];
char *filename_filter_list[MAX_NUMBER_OF_FILENAME_FILTERS];
int number_of_ignores=0;
int number_of_filename_filters=0;


int exit_pipe[2];

int daemon_pid;


void send_exit_signal(char code) {
  if (daemon_pid == 0 && o_wait) {
    if (write(exit_pipe[1], &code, 1) < 0)
      fprintf(stderr, "vmtouch: FATAL: write: %s", strerror(errno));
  }
}

void usage() {
  printf("\n");
  printf("vmtouch v%s - the Virtual Memory Toucher by Doug Hoyte\n", VMTOUCH_VERSION);
  printf("Portable file system cache diagnostics and control\n\n");
  printf("Usage: vmtouch [OPTIONS] ... FILES OR DIRECTORIES ...\n\nOptions:\n");
  printf("  -t touch pages into memory\n");
  printf("  -e evict pages from memory\n");
  printf("  -l lock pages in physical memory with mlock(2)\n");
  printf("  -L lock pages in physical memory with mlockall(2)\n");
  printf("  -d daemon mode\n");
  printf("  -m <size> max file size to touch\n");
  printf("  -p <range> use the specified portion instead of the entire file\n");
  printf("  -f follow symbolic links\n");
  printf("  -F don't crawl different filesystems\n");
  printf("  -h also count hardlinked copies\n");
  printf("  -i <pattern> ignores files and directories that match this pattern\n");
  printf("  -I <pattern> only process files that match this pattern\n");
  printf("  -b <list file> get files or directories from the list file\n");
  printf("  -0 in batch mode (-b) separate paths with NUL byte instead of newline\n");
  printf("  -w wait until all pages are locked (only useful together with -d)\n");
  printf("  -P <pidfile> write a pidfile (only useful together with -l or -L)\n");
  printf("  -o <type> output in machine friendly format.  'kv' for key=value pairs.\n");
  printf("  -v verbose\n");
  printf("  -q quiet\n");
  exit(1);
}

static void fatal(const char *fmt, ...) {
  va_list ap;
  char buf[4096];

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  fprintf(stderr, "vmtouch: FATAL: %s\n", buf);
  send_exit_signal(1);
  exit(1);
}

static void warning(const char *fmt, ...) {
  va_list ap;
  char buf[4096];

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (!o_quiet) fprintf(stderr, "vmtouch: WARNING: %s\n", buf);
}

static void reopen_all() {
  if (freopen("/dev/null", "r", stdin) == NULL ||
      freopen("/dev/null", "w", stdout) == NULL ||
      freopen("/dev/null", "w", stdout) == NULL)
    fatal("freopen: %s", strerror(errno));
}

static int wait_for_child() {
  int exit_read = 0;
  char exit_value = 0;
  int wait_status;

  while (1) {
    struct timeval tv;
    fd_set rfds;
    FD_ZERO(&rfds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    FD_SET(exit_pipe[0], &rfds);
    if (select(exit_pipe[0] + 1, &rfds, NULL, NULL, &tv) < 0)
      fatal("select: %s", strerror(errno));

    if (waitpid(daemon_pid, &wait_status, WNOHANG) > 0)
      fatal("daemon shut down unexpectedly");

    if (FD_ISSET(exit_pipe[0], &rfds))
      break;
  }
  exit_read = read(exit_pipe[0], &exit_value, 1);
  if (exit_read < 0)
    fatal("read: %s", strerror(errno));
  return exit_value;
}

void go_daemon() {
  daemon_pid = fork();
  if (daemon_pid == -1)
    fatal("fork: %s", strerror(errno));
  if (daemon_pid) {
    if (o_wait)
      exit(wait_for_child());
    exit(0);
  }

  if (setsid() == -1)
    fatal("setsid: %s", strerror(errno));

  if (!o_wait) reopen_all();
}


char *pretty_print_size(int64_t inp) {
  static char output[100];

  if (inp<1024) {
    snprintf(output, sizeof(output), "%" PRId64, inp);
    return output;
  }
  inp /= 1024;
  if (inp<1024) {
    snprintf(output, sizeof(output), "%" PRId64 "K", inp);
    return output;
  }
  inp /= 1024;
  if (inp<1024) {
    snprintf(output, sizeof(output), "%" PRId64 "M", inp);
    return output;
  }
  inp /= 1024;
  snprintf(output, sizeof(output), "%" PRId64 "G", inp);
  return output;
}

/*
 *  Convert ASCII string to int64_t number
 *  Note: The inp parameter can't be a character constant
 *        because it will be overwritten.
 */
int64_t parse_size(char *inp) {
  char *tp;
  int len=strlen(inp);
  char *errstr = "bad size. examples: 4096, 4k, 100M, 1.5G";
  char mult_char;
  int mult=1;
  double val;

  if (len < 1) fatal(errstr);

  mult_char = tolower(inp[len-1]);

  if (isalpha(mult_char)) {
    switch(mult_char) {
      case 'k': mult = 1024; break;
      case 'm': mult = 1024*1024; break;
      case 'g': mult = 1024*1024*1024; break;
      default: fatal("unknown size multiplier: %c", mult_char);
    }
    inp[len-1] = '\0';
  }

  val = strtod(inp, &tp);

  if (val < 0 || val == HUGE_VAL || *tp != '\0') fatal(errstr);

  val *= mult;

  if (val > INT64_MAX) fatal(errstr);

  return (int64_t) val;
}

int64_t bytes2pages(int64_t bytes) {
  return (bytes+pagesize-1) / pagesize;
}

void parse_range(char *inp) {
  char *token;
  int64_t upper_range=0;
  int64_t lower_range=0;

  token = strsep(&inp,"-");

  if (inp == NULL)
    upper_range = parse_size(token); // single value provided
  else {
    if (*token != '\0')
      lower_range = parse_size(token); // value before hyphen

    token = strsep(&inp,"-");
    if (*token != '\0')
      upper_range = parse_size(token); // value after hyphen

    if ((token = strsep(&inp,"-")) != NULL) fatal("malformed range: multiple hyphens");
  }

  // offset must be multiple of pagesize
  offset = (lower_range / pagesize) * pagesize;

  if (upper_range) {
    if (upper_range <= offset) fatal("range limits out of order");

    max_len = upper_range - offset;
  }
}


void parse_ignore_item(char *inp) {
  if (inp == NULL) {
    return;
  }

  if (strlen(inp) > MAX_FILENAME_LENGTH) {
    fatal("too long pattern provided to -i: %s", inp);
    return;
  }

  if (number_of_ignores >= MAX_NUMBER_OF_IGNORES) {
    fatal("too many patterns passed to -i. Max is %d", MAX_NUMBER_OF_IGNORES);
    return;
  }

  ignore_list[number_of_ignores] = strdup(inp);
  number_of_ignores++;
}

void parse_filename_filter_item(char *inp) {
  if (inp == NULL) {
    return;
  }

  if (strlen(inp) > MAX_FILENAME_LENGTH) {
    fatal("too long pattern provided to -I: %s", inp);
    return;
  }

  if (number_of_filename_filters >= MAX_NUMBER_OF_FILENAME_FILTERS) {
    fatal("too many patterns passed to -I. Max is %d", MAX_NUMBER_OF_FILENAME_FILTERS);
    return;
  }

  filename_filter_list[number_of_filename_filters] = strdup(inp);
  number_of_filename_filters++;
}


int aligned_p(void *p) {
  return 0 == ((long)p & (pagesize-1));
}

int is_mincore_page_resident(char p) {
  return p & 0x1;
}


void increment_nofile_rlimit() {
  struct rlimit r;

  if (getrlimit(RLIMIT_NOFILE, &r))
    fatal("increment_nofile_rlimit: getrlimit (%s)", strerror(errno));

  r.rlim_cur = r.rlim_max + 1;
  r.rlim_max = r.rlim_max + 1;

  if (setrlimit(RLIMIT_NOFILE, &r)) {
    if (errno == EPERM) {
      if (getuid() == 0 || geteuid() == 0) fatal("system open file limit reached");
      fatal("open file limit reached and unable to increase limit. retry as root");
    }
    fatal("increment_nofile_rlimit: setrlimit (%s)", strerror(errno));
  }
}



double gettimeofday_as_double() {
  struct timeval tv;
  gettimeofday(&tv, NULL);

  return tv.tv_sec + (tv.tv_usec/1000000.0);
}



void print_page_residency_chart(FILE *out, char *mincore_array, int64_t pages_in_file) {
  int64_t pages_in_core=0;
  int64_t pages_per_char;
  int64_t i,j=0,curr=0;

  if (pages_in_file <= RESIDENCY_CHART_WIDTH) pages_per_char = 1;
  else pages_per_char = (pages_in_file / RESIDENCY_CHART_WIDTH) + 1;

  fprintf(out, "\r[");

  for (i=0; i<pages_in_file; i++) {
    if (is_mincore_page_resident(mincore_array[i])) {
      curr++;
      pages_in_core++;
    }
    j++;
    if (j == pages_per_char) {
      if (curr == pages_per_char) fprintf(out, "O");
      else if (curr == 0) fprintf(out, " ");
      else fprintf(out, "o");

      j = curr = 0;
    }
  }

  if (j) {
    if (curr == j) fprintf(out, "O");
    else if (curr == 0) fprintf(out, " ");
    else fprintf(out, "o");
  }

  fprintf(out, "] %" PRId64 "/%" PRId64, pages_in_core, pages_in_file);

  fflush(out);
}


#ifdef __linux__
// check if mincore will report correctly, due side-channel vulnerabilities
// from 5.2+ it only reports if process has write permission to the file
// https://lwn.net/Articles/778437/
static int can_do_mincore(struct stat *st) {

  struct utsname utsinfo;
  if (uname(&utsinfo) == 0) {
    unsigned long ver[16];
    int i=0;
    char *p = utsinfo.release;
    while (*p) {
      if (isdigit(*p)) {
          ver[i] = strtol(p, &p, 10);
          i++;
      } else {
          p++;
      }
    }
    // kernal < 5.2
    if (ver[0]<5||ver[1]<2)
      return 1;
  }

  uid_t uid = getuid();
  return st->st_uid == uid ||
         (st->st_gid == getgid() && (st->st_mode&S_IWGRP)) ||
         (st->st_mode&S_IWOTH) ||
         uid == 0;
}
#endif


void vmtouch_file(char *path) {
  int fd = -1;
  void *mem = NULL;
  struct stat sb;
  int64_t len_of_file=0;
  int64_t len_of_range=0;
  int64_t pages_in_range;
  int i;
  int res;
  int open_flags;
  int mmap_flags = MAP_SHARED;

  retry_open:

  open_flags = O_RDONLY;

#if defined(O_NOATIME)
  open_flags |= O_NOATIME;
#endif

  fd = open(path, open_flags, 0);

#if defined(O_NOATIME)
  if (fd == -1 && errno == EPERM) {
    open_flags &= ~O_NOATIME;
    fd = open(path, open_flags, 0);
  }
#endif

  if (fd == -1) {
    if (errno == ENFILE || errno == EMFILE) {
      increment_nofile_rlimit();
      goto retry_open;
    }

    warning("unable to open %s (%s), skipping", path, strerror(errno));
    goto bail;
  }

  res = fstat(fd, &sb);

  if (res) {
    warning("unable to fstat %s (%s), skipping", path, strerror(errno));
    goto bail;
  }

  if (S_ISBLK(sb.st_mode)) {
#if defined(__linux__)
    if (ioctl(fd, BLKGETSIZE64, &len_of_file)) {
      warning("unable to ioctl %s (%s), skipping", path, strerror(errno));
      goto bail;
    }
#else
    fatal("discovering size of block devices not (yet?) supported on this platform");
#endif
  } else {
    len_of_file = sb.st_size;
  }


  if (len_of_file == 0) {
    goto bail;
  }

  if (len_of_file > o_max_file_size) {
    warning("file %s too large, skipping", path);
    goto bail;
  }


  if (max_len > 0 && (offset + max_len) < len_of_file) {
    len_of_range = max_len;
  } else if (offset >= len_of_file) {
    warning("file %s smaller than offset, skipping", path);
    goto bail;
  } else {
    len_of_range = len_of_file - offset;
  }

#ifdef __linux__
  if (o_lock && o_quiet) {
    if (mlockall(MCL_FUTURE))
      fatal("mlockall: %s (%s)", path, strerror(errno));
    mmap_flags |= MAP_POPULATE;
  }
#endif

  mem = mmap(NULL, len_of_range, PROT_READ, mmap_flags, fd, offset);

  if (mem == MAP_FAILED) {
    warning("unable to mmap file %s (%s), skipping", path, strerror(errno));
    goto bail;
  }

#ifdef __linux__
  if (o_lock && !o_quiet) {
    goto bail;
  }
#endif

  if (!aligned_p(mem)) fatal("mmap(%s) wasn't page aligned", path);

  pages_in_range = bytes2pages(len_of_range);

  total_pages += pages_in_range;

  if (o_evict) {
    if (o_verbose) printf("Evicting %s\n", path);

#if defined(__linux__) || defined(__hpux)
    if (posix_fadvise(fd, offset, len_of_range, POSIX_FADV_DONTNEED))
      warning("unable to posix_fadvise file %s (%s)", path, strerror(errno));
#elif defined(__FreeBSD__) || defined(__sun__) || defined(__APPLE__)
    if (msync(mem, len_of_range, MS_INVALIDATE))
      warning("unable to msync invalidate file %s (%s)", path, strerror(errno));
#else
    fatal("cache eviction not (yet?) supported on this platform");
#endif
  } else {
    double last_chart_print_time=0.0, temp_time;
    char *mincore_array = malloc(pages_in_range);
    if (mincore_array == NULL) fatal("Failed to allocate memory for mincore array (%s)", strerror(errno));

    // 3rd arg to mincore is char* on BSD and unsigned char* on linux
    if (mincore(mem, len_of_range, (void*)mincore_array)) fatal("mincore %s (%s)", path, strerror(errno));
    for (i=0; i<pages_in_range; i++) {
      if (is_mincore_page_resident(mincore_array[i])) {
        total_pages_in_core++;
      }
    }

    if (o_verbose) {
      printf("%s\n", path);
#ifdef __linux__
      if (!can_do_mincore(&sb)) {
        warning("Process does not have write permission, residency chart will not be accurate");
      }
#endif
      last_chart_print_time = gettimeofday_as_double();
      print_page_residency_chart(stdout, mincore_array, pages_in_range);
    }

    if (o_touch) {
      for (i=0; i<pages_in_range; i++) {
        junk_counter += ((char*)mem)[i*pagesize];
        mincore_array[i] = 1;

        if (o_verbose) {
          temp_time = gettimeofday_as_double();

          if (temp_time > (last_chart_print_time+CHART_UPDATE_INTERVAL)) {
            last_chart_print_time = temp_time;
            print_page_residency_chart(stdout, mincore_array, pages_in_range);
          }
        }
      }
    }

    if (o_verbose) {
      print_page_residency_chart(stdout, mincore_array, pages_in_range);
      printf("\n");
    }

    free(mincore_array);
  }

  if (o_lock) {
    if (mlock(mem, len_of_range))
      fatal("mlock: %s (%s)", path, strerror(errno));
  }

  bail:

  if (!o_lock && !o_lockall && mem) {
    if (munmap(mem, len_of_range)) warning("unable to munmap file %s (%s)", path, strerror(errno));
  }

  if (fd != -1) {
    close(fd);
  }
}


// compare device and inode information
int compare_func(const void *p1, const void *p2)
{
  const struct dev_and_inode *kp1 = p1, *kp2 = p2;
  int cmp1;
  cmp1 = (kp1->ino > kp2->ino) - (kp1->ino < kp2->ino);
  if (cmp1 != 0)
    return cmp1;
  return (kp1->dev > kp2->dev) - (kp1->dev < kp2->dev);
}

// add device and inode information to the tree of known inodes
static inline void add_object (struct stat *st)
{
  struct dev_and_inode *newp = malloc (sizeof (struct dev_and_inode));
  if (newp == NULL) {
    fatal("malloc: out of memory");
  }
  newp->dev = st->st_dev;
  newp->ino = st->st_ino;
  if (tsearch(newp, &seen_inodes, compare_func) == NULL) {
    fatal("tsearch: out of memory");
  }
}


int is_ignored(const char* path) {
  char *path_copy;
  int match, i;

  if (!number_of_ignores) return 0;

  path_copy = strdup(path);
  match = 0;

  char *filename = basename(path_copy);

  for (i = 0; i < number_of_ignores; i++) {
    if (fnmatch(ignore_list[i], filename, 0) == 0) {
      match = 1;
      break;
    }
  }

  free(path_copy);
  return match;
}


int is_filename_filtered(const char* path) {
  char *path_copy;
  int match, i;

  if (!number_of_filename_filters) return 1;

  path_copy = strdup(path);
  match = 0;

  char *filename = basename(path_copy);

  for (i = 0; i < number_of_filename_filters; i++) {
    if (fnmatch(filename_filter_list[i], filename, 0) == 0) {
      match = 1;
      break;
    }
  }

  free(path_copy);
  return match;
}



// return true only if the device and inode information has not been added before
static inline int find_object(struct stat *st)
{
  struct dev_and_inode obj;
  void *res;
  obj.dev = st->st_dev;
  obj.ino = st->st_ino;
  res = (void *) tfind(&obj, &seen_inodes, compare_func);
  return res != (void *) NULL;
}

void vmtouch_crawl(char *path) {
  struct stat sb;
  DIR *dirp;
  struct dirent *de;
  char npath[PATH_MAX];
  int res;
  int tp_path_len = strlen(path);
  int i;

  if (path[tp_path_len-1] == '/' && tp_path_len > 1) path[tp_path_len-1] = '\0'; // prevent ugly double slashes when printing path names

  if (is_ignored(path)) {
    return;
  }

  res = o_followsymlinks ? stat(path, &sb) : lstat(path, &sb);

  if (res) {
    warning("unable to stat %s (%s)", path, strerror(errno));
    return;
  } else {
    if (S_ISLNK(sb.st_mode)) {
      warning("not following symbolic link %s", path);
      return;
    }

    if (o_singlefilesystem) {
      if (!orig_device_inited) {
        orig_device = sb.st_dev;
        orig_device_inited = 1;
      } else {
        if (sb.st_dev != orig_device) {
          warning("not recursing into separate filesystem %s", path);
          return;
        }
      }
    }

    if (!o_ignorehardlinkeduplictes && sb.st_nlink > 1) {
      /*
       * For files with more than one link to it, ignore it if we already know
       * inode.  Without this check files copied as hardlinks (cp -al) are
       * counted twice (which may lead to a cache usage of more than 100% of
       * RAM).
       */
      if (find_object(&sb)) {
        // we already saw the device and inode referenced by this file
        return;
      } else {
        add_object(&sb);
      }
    }

    if (S_ISDIR(sb.st_mode)) {
      for (i=0; i<curr_crawl_depth; i++) {
        if (crawl_inodes[i] == sb.st_ino) {
          warning("symbolic link loop detected: %s", path);
          return;
        }
      }

      if (curr_crawl_depth == MAX_CRAWL_DEPTH)
        fatal("maximum directory crawl depth reached: %s", path);

      total_dirs++;

      crawl_inodes[curr_crawl_depth] = sb.st_ino;

      retry_opendir:

      dirp = opendir(path);

      if (dirp == NULL) {
        if (errno == ENFILE || errno == EMFILE) {
          increment_nofile_rlimit();
          goto retry_opendir;
        }

        warning("unable to opendir %s (%s), skipping", path, strerror(errno));
        return;
      }

      while((de = readdir(dirp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        if (snprintf(npath, sizeof(npath), "%s/%s", path, de->d_name) >= sizeof(npath)) {
          warning("path too long %s", path);
          goto bail;
        }

        curr_crawl_depth++;
        vmtouch_crawl(npath);
        curr_crawl_depth--;
      }

      bail:

      if (closedir(dirp)) {
        warning("unable to closedir %s (%s)", path, strerror(errno));
        return;
      }
    } else if (S_ISLNK(sb.st_mode)) {
      warning("not following symbolic link %s", path);
      return;
    } else if (S_ISREG(sb.st_mode) || S_ISBLK(sb.st_mode)) {
      if (is_filename_filtered(path)) {
        total_files++;
        vmtouch_file(path);
      }
    } else {
      warning("skipping non-regular file: %s", path);
    }
  }
}

static void vmtouch_batch_crawl(const char *path) {
  FILE *f;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  int delim = o_0_delim ? '\0' : '\n';

  if (!strcmp(path, "-")) {
    f = stdin;
  } else {
    f = fopen(path, "r");
    if (!f) {
      warning("unable to open %s (%s), skipping", path, strerror(errno));
      return;
    }
  }

  while ((read = getdelim(&line, &len, delim, f)) != -1) {
    // strip the newline character
    line[read-1] = '\0';
    vmtouch_crawl(line);
  }

  free(line);
  fclose(f);
}

static void remove_pidfile() {
  int res = 0;

  res = unlink(o_pidfile);
  if (res < 0 && errno != ENOENT) {
    warning("unable to remove pidfile %s (%s)", o_pidfile, strerror(errno));
  }
}

static void write_pidfile() {
  FILE *f = NULL;
  size_t wrote = 0;

  f = fopen(o_pidfile, "w");
  if (!f) {
    warning("unable to open pidfile %s (%s), skipping", o_pidfile, strerror(errno));
    return;
  }

  wrote = fprintf(f, "%d\n", getpid());

  fclose(f);

  if (wrote < 0) {
    warning("unable to write to pidfile %s (%s), deleting it", o_pidfile, strerror(errno));
    remove_pidfile();
  }
}

static void signal_handler_clear_pidfile(int signal_num) {
  remove_pidfile();
}

static void register_signals_for_pidfile() {
  struct sigaction sa = {0};
  sa.sa_handler = signal_handler_clear_pidfile;
  if (sigaction(SIGINT, &sa, NULL) < 0 ||
      sigaction(SIGTERM, &sa, NULL) < 0 ||
      sigaction(SIGQUIT, &sa, NULL) < 0) {
    warning("unable to register signals for pidfile (%s), skipping", strerror(errno));
  }
}






int main(int argc, char **argv) {
  int ch, i;
  char *prog = argv[0];
  struct timeval start_time;
  struct timeval end_time;

  if (pipe(exit_pipe))
    fatal("pipe: %s", strerror(errno));

  pagesize = sysconf(_SC_PAGESIZE);

  while((ch = getopt(argc, argv, "tevqlLdfFh0i:I:p:b:m:P:wo:")) != -1) {
    switch(ch) {
      case '?': usage(); break;
      case 't': o_touch = 1; break;
      case 'e': o_evict = 1; break;
      case 'q': o_quiet = 1; break;
      case 'v': o_verbose++; break;
      case 'l': o_lock = 1;
                o_touch = 1; break;
      case 'L': o_lockall = 1;
                o_touch = 1; break;
      case 'd': o_daemon = 1; break;
      case 'f': o_followsymlinks = 1; break;
      case 'F': o_singlefilesystem = 1; break;
      case 'h': o_ignorehardlinkeduplictes = 1; break;
      case 'p': parse_range(optarg); break;
      case 'i': parse_ignore_item(optarg); break;
      case 'I': parse_filename_filter_item(optarg); break;
      case 'm': {
        int64_t val = parse_size(optarg);
        o_max_file_size = (size_t) val;
        if (val != (int64_t) o_max_file_size) fatal("value for -m too big to fit in a size_t");
        break;
      }
      case 'w': o_wait = 1; break;
      case 'b': o_batch = optarg; break;
      case '0': o_0_delim = 1; break;
      case 'P': o_pidfile = optarg; break;
      case 'o': o_output = optarg; break;
    }
  }

  argc -= optind;
  argv += optind;

  if (o_touch) {
    if (o_evict) fatal("invalid option combination: -t and -e");
  }

  if (o_evict) {
    if (o_lock) fatal("invalid option combination: -e and -l");
  }

  if (o_lock && o_lockall) fatal("invalid option combination: -l and -L");

  if (o_daemon) {
    if (!(o_lock || o_lockall)) fatal("daemon mode must be combined with -l or -L");
    if (!o_wait) {
      o_quiet = 1;
      o_verbose = 0;
   }
  }

  if (o_wait && !o_daemon) fatal("wait mode must be combined with -d");

  if (o_quiet && o_verbose) fatal("invalid option combination: -q and -v");

  if (o_pidfile && (!o_lock && !o_lockall)) fatal("pidfile can only be created when -l or -L is specified");

  if (!argc && !o_batch) {
    printf("%s: no files or directories specified\n", prog);
    usage();
  }

  // Must be done now because mlock() not inherited across fork()
  if (o_daemon) go_daemon();

  gettimeofday(&start_time, NULL);

  if (o_batch) {
      vmtouch_batch_crawl(o_batch);
  }

  for (i=0; i<argc; i++) vmtouch_crawl(argv[i]);

  gettimeofday(&end_time, NULL);

  int64_t total_pages_in_core_size = total_pages_in_core * pagesize;
  int64_t total_pages_size         = total_pages * pagesize;
  double  total_pages_in_core_perc = 100.0*total_pages_in_core/total_pages;
  double  elapsed                  = (end_time.tv_sec - start_time.tv_sec) + (double)(end_time.tv_usec - start_time.tv_usec)/1000000.0;

  if (o_lock || o_lockall) {
    if (o_lockall) {
      if (mlockall(MCL_CURRENT))
        fatal("unable to mlockall (%s)", strerror(errno));
    }

    if (o_pidfile) {
      register_signals_for_pidfile();
      write_pidfile();
    }

    if (!o_quiet) printf("LOCKED %" PRId64 " pages (%s)\n", total_pages, pretty_print_size(total_pages_size));

    if (o_wait) reopen_all();

    send_exit_signal(0);
    select(0, NULL, NULL, NULL, NULL);
    exit(0);
  }

  if (!o_quiet) {
    if (o_output == NULL) {
      if (o_verbose) printf("\n");
      printf("           Files: %" PRId64 "\n", total_files);
      printf("     Directories: %" PRId64 "\n", total_dirs);
      if (o_touch)
        printf("   Touched Pages: %" PRId64 " (%s)\n", total_pages, pretty_print_size(total_pages_size));
      else if (o_evict)
        printf("   Evicted Pages: %" PRId64 " (%s)\n", total_pages, pretty_print_size(total_pages_size));
      else {
        printf("  Resident Pages: %" PRId64 "/%" PRId64 "  ", total_pages_in_core, total_pages);
        printf("%s/", pretty_print_size(total_pages_in_core_size));
        printf("%s  ", pretty_print_size(total_pages_size));
        if (total_pages)
          printf("%.3g%%", total_pages_in_core_perc);
        printf("\n");
      }
      printf("         Elapsed: %.5g seconds\n", elapsed);
    } else if (strncmp(o_output, "kv", 2) == 0) {
      char *desc = o_touch ? "Touched" :
                   o_evict ? "Evicted" :
                             "Resident";
      printf("Files=%" PRId64 " Directories=%" PRId64 " %sPages=%" PRId64 " TotalPages=%" PRId64 " %sSize=%" PRId64 " TotalSize=%" PRId64 " %sPercent=%.3g Elapsed=%.5g\n", 
        total_files, total_dirs, desc, total_pages_in_core, total_pages, desc, total_pages_in_core_size, total_pages_size, desc, total_pages_in_core_perc, elapsed);
    }
  }

  return 0;
}
