// Test-only interposer: emulate cold discovery and require it to stay off the UI thread.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Opaque return type avoids requiring GIO development headers for this standalone fixture.
typedef struct _GDesktopAppInfo GDesktopAppInfo;
typedef char*** (*search_fn)(const char*);
typedef GDesktopAppInfo* (*desktop_fn)(const char*);
static search_fn real_search;
static desktop_fn real_desktop;
static pthread_t main_thread;
static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static atomic_flag delayed = ATOMIC_FLAG_INIT;
static atomic_flag file_seen = ATOMIC_FLAG_INIT;

__attribute__((constructor)) static void remember_main_thread(void) {
  main_thread = pthread_self();
}

static void initialize_search(void) {
  real_search = (search_fn)dlsym(RTLD_NEXT, "g_desktop_app_info_search");
  real_desktop = (desktop_fn)dlsym(RTLD_NEXT, "g_desktop_app_info_new_from_filename");
  if (!real_search || !real_desktop) {
    fputs("Cannot find real desktop discovery functions\n", stderr);
    abort();
  }
}

static void require_worker(void) {
  if (pthread_equal(pthread_self(), main_thread)) {
    fputs("Desktop discovery ran on the UI thread\n", stderr);
    abort();
  }
}

GDesktopAppInfo* g_desktop_app_info_new_from_filename(const char* filename) {
  pthread_once(&initialized, initialize_search);
  require_worker();
  if (!atomic_flag_test_and_set_explicit(&file_seen, memory_order_relaxed)) {
    fputs("TASKBAR_TEST_DESKTOP_FILE\n", stderr);
    fflush(stderr);
  }
  return real_desktop(filename);
}

char*** g_desktop_app_info_search(const char* search_string) {
  pthread_once(&initialized, initialize_search);
  require_worker();
  if (strcmp(search_string, "unknown") == 0) {
    fputs("Unexpected synthetic app-ID lookup\n", stderr);
    abort();
  }
  if (!atomic_flag_test_and_set_explicit(&delayed, memory_order_relaxed)) {
    fputs("TASKBAR_TEST_SEARCH_DELAY_BEGIN\n", stderr);
    fflush(stderr);
    struct timespec remaining = {.tv_sec = 2};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
    fputs("TASKBAR_TEST_SEARCH_DELAY_END\n", stderr);
    fflush(stderr);
  }
  return real_search(search_string);
}
