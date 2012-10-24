/***********************************************************************
vmtouch - the Virtual Memory Toucher

Portable file system cache diagnostics and control

by Doug Hoyte (doug@hcsw.org)

Compilation:
  gcc -Wall -O3 -o vmtouch vmtouch.c

************************************************************************

Copyright (c) 2009,2012 Doug Hoyte and contributors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***********************************************************************/


#define VMTOUCH_VERSION "0.8.0"
#define RESIDENCY_CHART_WIDTH 60
#define CHART_UPDATE_INTERVAL 0.1
#define MAX_CRAWL_DEPTH 1024

#if defined(__linux__) || (defined(__hpux) && !defined(__LP64__))
// Make sure off_t is 64 bits on linux and when creating 32bit programs
// on HP-UX.
#define _FILE_OFFSET_BITS 64
#endif
#ifdef __linux__
// Required for posix_fadvise() on some linux systems
#define _XOPEN_SOURCE 600
// Required for mincore() on some linux systems
#define _BSD_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
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



long pagesize;

int64_t total_pages=0;
int64_t total_pages_in_core=0;
int64_t total_files=0;
int64_t total_dirs=0;

unsigned int junk_counter; // just to prevent any compiler optimizations

int curr_crawl_depth=0;
ino_t crawl_inodes[MAX_CRAWL_DEPTH];


int o_touch=0;
int o_evict=0;
int o_quiet=0;
int o_verbose=0;
int o_lock=0;
int o_lockall=0;
int o_daemon=0;
int o_followsymlinks=0;
size_t o_max_file_size=500*1024*1024;
int o_wait=0;

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
  printf("  -f follow symbolic links\n");
  printf("  -w wait until all pages are locked (only useful together with -d)\n");
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

  if (val <= 0 || *tp != '\0') fatal(errstr);

  return (int64_t) (mult*val);
}


int64_t bytes2pages(int64_t bytes) {
  return (bytes+pagesize-1) / pagesize;
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





void vmtouch_file(char *path) {
  int fd;
  void *mem;
  struct stat sb;
  int64_t len_of_file;
  int64_t pages_in_file;
  int i;
  int res;

  res = o_followsymlinks ? stat(path, &sb) : lstat(path, &sb);

  if (res) {
    warning("unable to stat %s (%s), skipping", path, strerror(errno));
    return;
  }

  if (S_ISLNK(sb.st_mode)) {
    warning("not following symbolic link %s", path);
    return;
  }

  if (sb.st_size == 0) return;

  if (sb.st_size > o_max_file_size) {
    warning("file %s too large, skipping", path);
    return;
  }

  len_of_file = sb.st_size;

  retry_open:

  fd = open(path, O_RDONLY, 0);

  if (fd == -1) {
    if (errno == ENFILE || errno == EMFILE) {
      increment_nofile_rlimit();
      goto retry_open;
    }

    warning("unable to open %s (%s), skipping", path, strerror(errno));
    return;
  }

  mem = mmap(NULL, len_of_file, PROT_READ, MAP_SHARED, fd, 0);

  if (mem == MAP_FAILED) {
    warning("unable to mmap file %s (%s), skipping", path, strerror(errno));
    close(fd);
    return;
  }

  if (!aligned_p(mem)) fatal("mmap(%s) wasn't page aligned", path);

  pages_in_file = bytes2pages(len_of_file);

  total_pages += pages_in_file;

  if (o_evict) {
    if (o_verbose) printf("Evicting %s\n", path);

#if defined(__linux__) || defined(__hpux)
    if (posix_fadvise(fd, 0, len_of_file, POSIX_FADV_DONTNEED))
      warning("unable to posix_fadvise file %s (%s)", path, strerror(errno));
#elif defined(__FreeBSD__) || defined(__sun__) || defined(__APPLE__)
    if (msync(mem, len_of_file, MS_INVALIDATE))
      warning("unable to msync invalidate file %s (%s)", path, strerror(errno));
#else
    fatal("cache eviction not (yet?) supported on this platform");
#endif
  } else {
    int64_t pages_in_core=0;
    double last_chart_print_time=0.0, temp_time;
    char *mincore_array = malloc(pages_in_file);
    if (mincore_array == NULL) fatal("Failed to allocate memory for mincore array (%s)", strerror(errno));

    // 3rd arg to mincore is char* on BSD and unsigned char* on linux
    if (mincore(mem, len_of_file, (void*)mincore_array)) fatal("mincore %s (%s)", path, strerror(errno));
    for (i=0; i<pages_in_file; i++) {
      if (is_mincore_page_resident(mincore_array[i])) {
        pages_in_core++;
        total_pages_in_core++;
      }
    }

    if (o_verbose) {
      printf("%s\n", path);
      last_chart_print_time = gettimeofday_as_double();
      print_page_residency_chart(stdout, mincore_array, pages_in_file);
    }

    if (o_touch) {
      for (i=0; i<pages_in_file; i++) {
        junk_counter += ((char*)mem)[i*pagesize];
        mincore_array[i] = 1;

        if (o_verbose) {
          temp_time = gettimeofday_as_double();

          if (temp_time > (last_chart_print_time+CHART_UPDATE_INTERVAL)) {
            last_chart_print_time = temp_time;
            print_page_residency_chart(stdout, mincore_array, pages_in_file);
          }
        }
      }
    }

    if (o_verbose) {
      print_page_residency_chart(stdout, mincore_array, pages_in_file);
      printf("\n");
    }

    free(mincore_array);
  }

  if (o_lock) {
    if (mlock(mem, len_of_file))
      fatal("mlock: %s (%s)", path, strerror(errno));
  }

  if (!o_lock && !o_lockall) {
    if (munmap(mem, len_of_file)) warning("unable to munmap file %s (%s)", path, strerror(errno));
    close(fd);
  }
}







void vmtouch_crawl(char *path) {
  struct stat sb;
  DIR *dirp;
  struct dirent *de;
  char npath[PATH_MAX];
  int res;
  int tp_path_len = strlen(path);
  int i;

  if (path[tp_path_len-1] == '/') path[tp_path_len-1] = '\0'; // prevent ugly double slashes when printing path names

  res = o_followsymlinks ? stat(path, &sb) : lstat(path, &sb);

  if (res) {
    warning("unable to stat %s (%s)", path, strerror(errno));
    return;
  } else {
    if (S_ISLNK(sb.st_mode)) {
      warning("not following symbolic link %s", path);
      return;
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
    } else if (S_ISREG(sb.st_mode)) {
      total_files++;
      vmtouch_file(path);
    } else {
      warning("skipping non-regular file: %s", path);
    }
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

  while((ch = getopt(argc, argv,"tevqlLdfpb:m:w")) != -1) {
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
      case 'p':
        o_touch = 1;
        printf("%d %s %ld\n", sizeof(void*) == 4 ? 32 : 64,
                              ((char*)&o_touch)[0] ? "little" : "big",
                              pagesize);
        fprintf(stderr, "-p is DEPRECATED. This switch will do something else in a future vmtouch release.\n");
        exit(0);
      case 'm': {
        int64_t val = parse_size(optarg);
        o_max_file_size = (size_t) val;
        if (val != (int64_t) o_max_file_size) fatal("value for -m too big to fit in a size_t");
        break;
      }
      case 'w': o_wait = 1; break;
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

  if (!argc) {
    printf("%s: no files or directories specified\n", prog);
    usage();
  }

  // Must be done now because mlock() not inherited across fork()
  if (o_daemon) go_daemon();

  gettimeofday(&start_time, NULL);

  for (i=0; i<argc; i++) vmtouch_crawl(argv[i]);

  gettimeofday(&end_time, NULL);

  if (o_lock || o_lockall) {
    if (o_lockall) {
      if (mlockall(MCL_CURRENT))
        fatal("unable to mlockall (%s)", strerror(errno));
    }

    if (!o_quiet) printf("LOCKED %" PRId64 " pages (%s)\n", total_pages, pretty_print_size(total_pages*pagesize));

    if (o_wait) reopen_all();

    send_exit_signal(0);
    select(0, NULL, NULL, NULL, NULL);
    exit(0);
  }

  if (!o_quiet) {
    if (o_verbose) printf("\n");
    printf("           Files: %" PRId64 "\n", total_files);
    printf("     Directories: %" PRId64 "\n", total_dirs);
    if (o_touch)
      printf("   Touched Pages: %" PRId64 " (%s)\n", total_pages, pretty_print_size(total_pages*pagesize));
    else if (o_evict)
      printf("   Evicted Pages: %" PRId64 " (%s)\n", total_pages, pretty_print_size(total_pages*pagesize));
    else {
      printf("  Resident Pages: %" PRId64 "/%" PRId64 "  ", total_pages_in_core, total_pages);
      printf("%s/", pretty_print_size(total_pages_in_core*pagesize));
      printf("%s  ", pretty_print_size(total_pages*pagesize));
      printf(total_pages ? "%.3g%%\n" : "\n", 100.0*total_pages_in_core/total_pages);
    }
    printf("         Elapsed: %.5g seconds\n", (end_time.tv_sec - start_time.tv_sec) + (double)(end_time.tv_usec - start_time.tv_usec)/1000000.0);
  }

  return 0;
}
