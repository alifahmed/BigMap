/*
   american fuzzy lop++ - map display utility
   ------------------------------------------

   Originally written by Michal Zalewski

   Forkserver design by Jann Horn <jannhorn@googlemail.com>

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   A very simple tool that runs the targeted binary and displays
   the contents of the trace bitmap in a human-readable form. Useful in
   scripts to eliminate redundant inputs and perform other checks.

   Exit code is 2 if the target program crashes; 1 if it times out or
   there is a problem executing it; or 0 if execution is successful.

 */

#define AFL_MAIN

#ifdef __ANDROID__
#include "android-ashmem.h"
#endif
#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"
#include "hash.h"
#include "sharedmem.h"
#include "forkserver.h"
#include "common.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>

#include <sys/wait.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>

u8* trace_bits;                        /* SHM with instrumentation bitmap   */
u32 *trace_idx;
u32 map_used;

s32 forksrv_pid,                       /* PID of the fork server            */
    child_pid;                         /* PID of the tested program         */

s32 fsrv_ctl_fd,                       /* Fork server control pipe (write)  */
    fsrv_st_fd;                        /* Fork server status pipe (read)    */

s32 out_fd;                            /* Persistent fd for out_file        */
s32 dev_null_fd = -1;                  /* FD to /dev/null                   */

s32   out_fd = -1, out_dir_fd = -1, dev_urandom_fd = -1;
FILE* plot_file;
u8    uses_asan;

u8 *out_file,                          /* Trace output file                 */
    *in_dir,                           /* input folder                      */
    *doc_path,                         /* Path to docs                      */
    *at_file;                          /* Substitution string for @@        */

static u8* in_data;                    /* Input data                        */

u32 exec_tmout;                        /* Exec timeout (ms)                 */

static u32 total, highest;             /* tuple content information         */

static u32 in_len,                     /* Input data length                 */
    arg_offset,
    total_execs;                       /* Total number of execs             */

u64 mem_limit = MEM_LIMIT;             /* Memory limit (MB)                 */

u8 quiet_mode,                         /* Hide non-essential messages?      */
    edges_only,                        /* Ignore hit counts?                */
    raw_instr_output,                  /* Do not apply AFL filters          */
    cmin_mode,                         /* Generate output in afl-cmin mode? */
    binary_mode,                       /* Write output as a binary map      */
    use_stdin = 1,                     /* use stdin - unused here           */
    keep_cores;                        /* Allow coredumps?                  */

static volatile u8 stop_soon,          /* Ctrl-C pressed?                   */
    child_timed_out,                   /* Child timed out?                  */
    child_crashed;                     /* Child crashed?                    */

static u8 qemu_mode;

/* Classify tuple counts. Instead of mapping to individual bits, as in
   afl-fuzz.c, we map to more user-friendly numbers between 1 and 8. */

static const u8 count_class_human[256] = {

    [0] = 0,          [1] = 1,        [2] = 2,         [3] = 3,
    [4 ... 7] = 4,    [8 ... 15] = 5, [16 ... 31] = 6, [32 ... 127] = 7,
    [128 ... 255] = 8

};

static const u8 count_class_binary[256] = {

    [0] = 0,
    [1] = 1,
    [2] = 2,
    [3] = 4,
    [4 ... 7] = 8,
    [8 ... 15] = 16,
    [16 ... 31] = 32,
    [32 ... 127] = 64,
    [128 ... 255] = 128

};

static void classify_counts(u8* mem, const u8* map) {

  u32 i = MAP_SIZE;

  if (edges_only) {

    while (i--) {

      if (*mem) *mem = 1;
      mem++;

    }

  } else if (!raw_instr_output) {

    while (i--) {

      *mem = map[*mem];
      mem++;

    }

  }

}

/* Write results. */

static u32 write_results_to_file(u8 *out_file) {

  s32 fd;
  u32 i, ret = 0;

  u8 cco = !!getenv("AFL_CMIN_CRASHES_ONLY"),
     caa = !!getenv("AFL_CMIN_ALLOW_ANY");

  if (!strncmp(out_file, "/dev/", 5)) {

    fd = open(out_file, O_WRONLY, 0600);
    if (fd < 0) PFATAL("Unable to open '%s'", out_file);

  } else if (!strcmp(out_file, "-")) {

    fd = dup(1);
    if (fd < 0) PFATAL("Unable to open stdout");

  } else {

    unlink(out_file);                                      /* Ignore errors */
    fd = open(out_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) PFATAL("Unable to create '%s'", out_file);

  }

  if (binary_mode) {

    for (i = 0; i < MAP_SIZE; i++)
      if (trace_bits[i]) ret++;

    ck_write(fd, trace_bits, MAP_SIZE, out_file);
    close(fd);

  } else {

    FILE* f = fdopen(fd, "w");

    if (!f) PFATAL("fdopen() failed");

    for (i = 0; i < MAP_SIZE; i++) {

      if (!trace_bits[i]) continue;
      ret++;

      total += trace_bits[i];
      if (highest < trace_bits[i]) highest = trace_bits[i];

      if (cmin_mode) {

        if (child_timed_out) break;
        if (!caa && child_crashed != cco) break;

        fprintf(f, "%u%u\n", trace_bits[i], i);

      } else

        fprintf(f, "%06u:%u\n", i, trace_bits[i]);

    }

    fclose(f);

  }

  return ret;

}

/* Write results. */

static u32 write_results(void) {

  return write_results_to_file(out_file);
  
}

/* Write output file. */

static s32 write_to_file(u8* path, u8* mem, u32 len) {

  s32 ret;

  unlink(path);                                            /* Ignore errors */

  ret = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);

  if (ret < 0) PFATAL("Unable to create '%s'", path);

  ck_write(ret, mem, len, path);

  lseek(ret, 0, SEEK_SET);

  return ret;

}

/* Write modified data to file for testing. If use_stdin is clear, the old file
   is unlinked and a new one is created. Otherwise, out_fd is rewound and
   truncated. */

static void write_to_testcase(void* mem, u32 len) {

  if (use_stdin) {

    lseek(0, 0, SEEK_SET);

    ck_write(0, mem, len, out_file);

    if (ftruncate(0, len)) PFATAL("ftruncate() failed");
    lseek(0, 0, SEEK_SET);

  }

}

/* Execute target application. Returns 0 if the changes are a dud, or
   1 if they should be kept. */

static u8 run_target_forkserver(char** argv, u8* mem, u32 len) {

  static struct itimerval it;
  static u32              prev_timed_out = 0;
  int                     status = 0;

  memset(trace_bits, 0, MAP_SIZE);
  MEM_BARRIER();

  write_to_testcase(mem, len);

  s32 res;

  /* we have the fork server up and running, so simply
     tell it to have at it, and then read back PID. */

  if ((res = write(fsrv_ctl_fd, &prev_timed_out, 4)) != 4) {

    if (stop_soon) return 0;
    RPFATAL(res, "Unable to request new process from fork server (OOM?)");

  }

  if ((res = read(fsrv_st_fd, &child_pid, 4)) != 4) {

    if (stop_soon) return 0;
    RPFATAL(res, "Unable to request new process from fork server (OOM?)");

  }

  if (child_pid <= 0) FATAL("Fork server is misbehaving (OOM?)");

  /* Configure timeout, wait for child, cancel timeout. */

  if (exec_tmout) {

    it.it_value.tv_sec = (exec_tmout / 1000);
    it.it_value.tv_usec = (exec_tmout % 1000) * 1000;

  }

  setitimer(ITIMER_REAL, &it, NULL);

  if ((res = read(fsrv_st_fd, &status, 4)) != 4) {

    if (stop_soon) return 0;
    RPFATAL(res, "Unable to communicate with fork server (OOM?)");

  }

  child_pid = 0;
  it.it_value.tv_sec = 0;
  it.it_value.tv_usec = 0;

  setitimer(ITIMER_REAL, &it, NULL);

  MEM_BARRIER();

  /* Clean up bitmap, analyze exit condition, etc. */

  if (*(u32*)trace_bits == EXEC_FAIL_SIG)
    FATAL("Unable to execute '%s'", argv[0]);

  classify_counts(trace_bits,
                  binary_mode ? count_class_binary : count_class_human);
  total_execs++;

  if (stop_soon) {

    SAYF(cRST cLRD "\n+++ afl-showmap folder mode aborted by user +++\n" cRST);
    close(write_to_file(out_file, in_data, in_len));
    exit(1);

  }

  /* Always discard inputs that time out. */

  if (child_timed_out) { return 0; }

  /* Handle crashing inputs depending on current mode. */

  if (WIFSIGNALED(status) ||
      (WIFEXITED(status) && WEXITSTATUS(status) == MSAN_ERROR) ||
      (WIFEXITED(status) && WEXITSTATUS(status))) {

    return 0;

  }

  return 0;

}

/* Read initial file. */

u32 read_file(u8 *in_file) {

  struct stat st;
  s32         fd = open(in_file, O_RDONLY);

  if (fd < 0) WARNF("Unable to open '%s'", in_file);

  if (fstat(fd, &st) || !st.st_size) WARNF("Zero-sized input file '%s'.", in_file);

  in_len = st.st_size;
  in_data = ck_alloc_nozero(in_len);

  ck_read(fd, in_data, in_len, in_file);

  close(fd);

  //OKF("Read %u byte%s from '%s'.", in_len, in_len == 1 ? "" : "s", in_file);

  return in_len;
}

/* Execute target application. */

static void run_target(char** argv) {

  static struct itimerval it;
  int                     status = 0;

  if (!quiet_mode) SAYF("-- Program output begins --\n" cRST);

  MEM_BARRIER();

  child_pid = fork();

  if (child_pid < 0) PFATAL("fork() failed");

  if (!child_pid) {

    struct rlimit r;

    if (quiet_mode) {

      s32 fd = open("/dev/null", O_RDWR);

      if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) {

        *(u32*)trace_bits = EXEC_FAIL_SIG;
        PFATAL("Descriptor initialization failed");

      }

      close(fd);

    }

    if (mem_limit) {

      r.rlim_max = r.rlim_cur = ((rlim_t)mem_limit) << 20;

#ifdef RLIMIT_AS

      setrlimit(RLIMIT_AS, &r);                            /* Ignore errors */

#else

      setrlimit(RLIMIT_DATA, &r);                          /* Ignore errors */

#endif                                                        /* ^RLIMIT_AS */

    }

    if (!keep_cores)
      r.rlim_max = r.rlim_cur = 0;
    else
      r.rlim_max = r.rlim_cur = RLIM_INFINITY;

    setrlimit(RLIMIT_CORE, &r);                            /* Ignore errors */

    if (!getenv("LD_BIND_LAZY")) setenv("LD_BIND_NOW", "1", 0);

    setsid();

    execv(target_path, argv);

    *(u32*)trace_bits = EXEC_FAIL_SIG;
    exit(0);

  }

  /* Configure timeout, wait for child, cancel timeout. */

  if (exec_tmout) {

    child_timed_out = 0;
    it.it_value.tv_sec = (exec_tmout / 1000);
    it.it_value.tv_usec = (exec_tmout % 1000) * 1000;

  }

  setitimer(ITIMER_REAL, &it, NULL);

  if (waitpid(child_pid, &status, 0) <= 0) FATAL("waitpid() failed");

  child_pid = 0;
  it.it_value.tv_sec = 0;
  it.it_value.tv_usec = 0;
  setitimer(ITIMER_REAL, &it, NULL);

  MEM_BARRIER();

  /* Clean up bitmap, analyze exit condition, etc. */

  if (*(u32*)trace_bits == EXEC_FAIL_SIG)
    FATAL("Unable to execute '%s'", argv[0]);

  classify_counts(trace_bits,
                  binary_mode ? count_class_binary : count_class_human);

  if (!quiet_mode) SAYF(cRST "-- Program output ends --\n");

  if (!child_timed_out && !stop_soon && WIFSIGNALED(status)) child_crashed = 1;

  if (!quiet_mode) {

    if (child_timed_out)
      SAYF(cLRD "\n+++ Program timed off +++\n" cRST);
    else if (stop_soon)
      SAYF(cLRD "\n+++ Program aborted by user +++\n" cRST);
    else if (child_crashed)
      SAYF(cLRD "\n+++ Program killed by signal %u +++\n" cRST,
           WTERMSIG(status));

  }

}

/* Handle Ctrl-C and the like. */

static void handle_stop_sig(int sig) {

  stop_soon = 1;

  if (child_pid > 0) kill(child_pid, SIGKILL);

}

/* Do basic preparations - persistent fds, filenames, etc. */

static void set_up_environment(void) {

  setenv("ASAN_OPTIONS",
         "abort_on_error=1:"
         "detect_leaks=0:"
         "symbolize=0:"
         "allocator_may_return_null=1",
         0);

  setenv("MSAN_OPTIONS", "exit_code=" STRINGIFY(MSAN_ERROR) ":"
                         "symbolize=0:"
                         "abort_on_error=1:"
                         "allocator_may_return_null=1:"
                         "msan_track_origins=0", 0);

  if (getenv("AFL_PRELOAD")) {

    if (qemu_mode) {

      u8* qemu_preload = getenv("QEMU_SET_ENV");
      u8* afl_preload = getenv("AFL_PRELOAD");
      u8* buf;

      s32 i, afl_preload_size = strlen(afl_preload);
      for (i = 0; i < afl_preload_size; ++i) {

        if (afl_preload[i] == ',')
          PFATAL(
              "Comma (',') is not allowed in AFL_PRELOAD when -Q is "
              "specified!");

      }

      if (qemu_preload)
        buf = alloc_printf("%s,LD_PRELOAD=%s", qemu_preload, afl_preload);
      else
        buf = alloc_printf("LD_PRELOAD=%s", afl_preload);

      setenv("QEMU_SET_ENV", buf, 1);

      ck_free(buf);

    } else {

      setenv("LD_PRELOAD", getenv("AFL_PRELOAD"), 1);
      setenv("DYLD_INSERT_LIBRARIES", getenv("AFL_PRELOAD"), 1);

    }

  }

}

/* Setup signal handlers, duh. */

static void setup_signal_handlers(void) {

  struct sigaction sa;

  sa.sa_handler = NULL;
  sa.sa_flags = SA_RESTART;
  sa.sa_sigaction = NULL;

  sigemptyset(&sa.sa_mask);

  /* Various ways of saying "stop". */

  sa.sa_handler = handle_stop_sig;
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  /* Exec timeout notifications. */

  sa.sa_handler = handle_timeout;
  sigaction(SIGALRM, &sa, NULL);

}

/* Show banner. */

static void show_banner(void) {

  SAYF(cCYA "afl-showmap" VERSION cRST " by Michal Zalewski\n");

}

/* Display usage hints. */

static void usage(u8* argv0) {

  show_banner();

  SAYF(
      "\n%s [ options ] -- /path/to/target_app [ ... ]\n\n"

      "Required parameters:\n\n"

      "  -o file       - file to write the trace data to\n\n"

      "Execution control settings:\n\n"

      "  -t msec       - timeout for each run (none)\n"
      "  -m megs       - memory limit for child process (%d MB)\n"
      "  -Q            - use binary-only instrumentation (QEMU mode)\n"
      "  -U            - use Unicorn-based instrumentation (Unicorn mode)\n"
      "  -W            - use qemu-based instrumentation with Wine (Wine mode)\n"
      "                  (Not necessary, here for consistency with other afl-* "
      "tools)\n\n"

      "Other settings:\n\n"

      "  -i dir        - process all files in this directory, -o must be a directory\n"
      "                  and each bitmap will be written there individually.\n"
      "  -q            - sink program's output and don't show messages\n"
      "  -e            - show edge coverage only, ignore hit counts\n"
      "  -r            - show real tuple values instead of AFL filter values\n"
      "  -c            - allow core dumps\n\n"

      "This tool displays raw tuple data captured by AFL instrumentation.\n"
      "For additional help, consult %s/README.\n\n" cRST,

      argv0, MEM_LIMIT, doc_path);

  exit(1);

}

/* Find binary. */

static void find_binary(u8* fname) {

  u8*         env_path = 0;
  struct stat st;

  if (strchr(fname, '/') || !(env_path = getenv("PATH"))) {

    target_path = ck_strdup(fname);

    if (stat(target_path, &st) || !S_ISREG(st.st_mode) ||
        !(st.st_mode & 0111) || st.st_size < 4)
      FATAL("Program '%s' not found or not executable", fname);

  } else {

    while (env_path) {

      u8 *cur_elem, *delim = strchr(env_path, ':');

      if (delim) {

        cur_elem = ck_alloc(delim - env_path + 1);
        memcpy(cur_elem, env_path, delim - env_path);
        delim++;

      } else

        cur_elem = ck_strdup(env_path);

      env_path = delim;

      if (cur_elem[0])
        target_path = alloc_printf("%s/%s", cur_elem, fname);
      else
        target_path = ck_strdup(fname);

      ck_free(cur_elem);

      if (!stat(target_path, &st) && S_ISREG(st.st_mode) &&
          (st.st_mode & 0111) && st.st_size >= 4)
        break;

      ck_free(target_path);
      target_path = 0;

    }

    if (!target_path) FATAL("Program '%s' not found or not executable", fname);

  }

}

/* Main entry point */
extern u8 disable_hugepage;

int main(int argc, char** argv) {

  s32    opt, i;
  u8     mem_limit_given = 0, timeout_given = 0, unicorn_mode = 0, use_wine = 0;
  u32    tcnt = 0;
  char** use_argv;

  doc_path = access(DOC_PATH, F_OK) ? "docs" : DOC_PATH;
  disable_hugepage = 1;
  while ((opt = getopt(argc, argv, "+i:o:f:m:t:A:eqZQUWbcrh")) > 0)

    switch (opt) {

      case 'i':
        if (in_dir) FATAL("Multiple -i options not supported");
        in_dir = optarg;
        break;

      case 'o':

        if (out_file) FATAL("Multiple -o options not supported");
        out_file = optarg;
        break;

      case 'm': {

        u8 suffix = 'M';

        if (mem_limit_given) FATAL("Multiple -m options not supported");
        mem_limit_given = 1;

        if (!strcmp(optarg, "none")) {

          mem_limit = 0;
          break;

        }

        if (sscanf(optarg, "%llu%c", &mem_limit, &suffix) < 1 ||
            optarg[0] == '-')
          FATAL("Bad syntax used for -m");

        switch (suffix) {

          case 'T': mem_limit *= 1024 * 1024; break;
          case 'G': mem_limit *= 1024; break;
          case 'k': mem_limit /= 1024; break;
          case 'M': break;

          default: FATAL("Unsupported suffix or bad syntax for -m");

        }

        if (mem_limit < 5) FATAL("Dangerously low value of -m");

        if (sizeof(rlim_t) == 4 && mem_limit > 2000)
          FATAL("Value of -m out of range on 32-bit systems");

      }

      break;

      case 'f':  // only in here to avoid a compiler warning for use_stdin

        use_stdin = 0;
        FATAL("Option -f is not supported in afl-showmap");

        break;

      case 't':

        if (timeout_given) FATAL("Multiple -t options not supported");
        timeout_given = 1;

        if (strcmp(optarg, "none")) {

          exec_tmout = atoi(optarg);

          if (exec_tmout < 20 || optarg[0] == '-')
            FATAL("Dangerously low value of -t");

        }

        break;

      case 'e':

        if (edges_only) FATAL("Multiple -e options not supported");
        if (raw_instr_output) FATAL("-e and -r are mutually exclusive");
        edges_only = 1;
        break;

      case 'q':

        if (quiet_mode) FATAL("Multiple -q options not supported");
        quiet_mode = 1;
        break;

      case 'Z':

        /* This is an undocumented option to write data in the syntax expected
           by afl-cmin. Nobody else should have any use for this. */

        cmin_mode = 1;
        quiet_mode = 1;
        break;

      case 'A':
        /* Another afl-cmin specific feature. */
        at_file = optarg;
        break;

      case 'Q':

        if (qemu_mode) FATAL("Multiple -Q options not supported");
        if (!mem_limit_given) mem_limit = MEM_LIMIT_QEMU;

        qemu_mode = 1;
        break;

      case 'U':

        if (unicorn_mode) FATAL("Multiple -U options not supported");
        if (!mem_limit_given) mem_limit = MEM_LIMIT_UNICORN;

        unicorn_mode = 1;
        break;

      case 'W':                                           /* Wine+QEMU mode */

        if (use_wine) FATAL("Multiple -W options not supported");
        qemu_mode = 1;
        use_wine = 1;

        if (!mem_limit_given) mem_limit = 0;

        break;

      case 'b':

        /* Secret undocumented mode. Writes output in raw binary format
           similar to that dumped by afl-fuzz in <out_dir/queue/fuzz_bitmap. */

        binary_mode = 1;
        break;

      case 'c':

        if (keep_cores) FATAL("Multiple -c options not supported");
        keep_cores = 1;
        break;

      case 'r':

        if (raw_instr_output) FATAL("Multiple -r options not supported");
        if (edges_only) FATAL("-e and -r are mutually exclusive");
        raw_instr_output = 1;
        break;

      case 'h':
        usage(argv[0]);
        return -1;
        break;

      default: usage(argv[0]);

    }

  if (optind == argc || !out_file) usage(argv[0]);

  setup_shm(0);
  setup_signal_handlers();

  set_up_environment();

  find_binary(argv[optind]);

  if (!quiet_mode) {

    show_banner();
    ACTF("Executing '%s'...\n", target_path);

  }

  if (in_dir)  {
  
    if (at_file) PFATAL("Options -A and -i are mutually exclusive");
    at_file = "@@";
  
  }

  detect_file_args(argv + optind, at_file);
  
  for (i = optind; i < argc; i++)
    if (strcmp(argv[i], "@@") == 0)
      arg_offset = i;

  if (qemu_mode) {

    if (use_wine)
      use_argv = get_wine_argv(argv[0], argv + optind, argc - optind);
    else
      use_argv = get_qemu_argv(argv[0], argv + optind, argc - optind);

  } else

    use_argv = argv + optind;

  if (in_dir) {

    DIR *dir_in, *dir_out;
    struct dirent* dir_ent;
    int  done = 0;
    u8 infile[4096], outfile[4096];

    dev_null_fd = open("/dev/null", O_RDWR);
    if (dev_null_fd < 0) PFATAL("Unable to open /dev/null");

    if (!(dir_in = opendir(in_dir))) PFATAL("cannot open directory %s", in_dir);

    if (!(dir_out = opendir(out_file)))
      if (mkdir(out_file, 0700))
        PFATAL("cannot create output directory %s", out_file);

    if (arg_offset) argv[arg_offset] = infile;

    init_forkserver(use_argv);

    while (done == 0 && (dir_ent = readdir(dir_in))) {

      if (dir_ent->d_name[0] == '.') continue; // skip anything that starts with '.'
      if (dir_ent->d_type != DT_REG) continue; // only regular files

      snprintf(infile, sizeof(infile), "%s/%s", in_dir, dir_ent->d_name);
      snprintf(outfile, sizeof(outfile), "%s/%s", out_file, dir_ent->d_name);

      if (read_file(infile)) {
        run_target_forkserver(use_argv, in_data, in_len);
        ck_free(in_data);
        tcnt = write_results_to_file(outfile);
      }

    }

  } else {

    run_target(use_argv);
    tcnt = write_results();

  }

  if (!quiet_mode) {

    if (!tcnt) FATAL("No instrumentation detected" cRST);
    OKF("Captured %u tuples (highest value %u, total values %u) in '%s'." cRST,
        tcnt, highest, total, out_file);

  }

  exit(child_crashed * 2 + child_timed_out);

}
