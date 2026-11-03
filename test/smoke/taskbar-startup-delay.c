// Test-only interposer: emulate a cold desktop-file search without dropping system caches.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef char*** (*search_fn)(const char*);
static search_fn real_search;
static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static atomic_flag delayed = ATOMIC_FLAG_INIT;

static void initialize_search(void) {
  real_search = (search_fn)dlsym(RTLD_NEXT, "g_desktop_app_info_search");
  if (!real_search) {
    fputs("Cannot find real desktop search\n", stderr);
    abort();
  }
}

char*** g_desktop_app_info_search(const char* search_string) {
  pthread_once(&initialized, initialize_search);
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
