/**
 * @file
 *
 * The dlopen probe (make check-dlopen). It is not linked against the
 * library: it loads it with dlopen, finds its entry points with dlsym, and
 * enters and leaves activation records with them; then, in a second dlopen
 * of its own after closing the first, it loads a module that is linked against the library (the way
 * a host loads an engine) and has the module do the same. Either fails if the
 * library's thread-local cannot be reached from a library loaded at run time.
 *
 * Usage: probe <library.so> <module.so>
 */

#define _POSIX_C_SOURCE 200809L

#include "exercise.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

/* The namespaced name, as the header's renaming macro spells it. */
#define PROBE_STR2(x) #x
#define PROBE_STR(x) PROBE_STR2(x)

static int missing = 0;

static void find(void * handle, const char * name, void * out, size_t size) {
  void * symbol = dlsym(handle, name);
  if (symbol == NULL || size != sizeof symbol) {
    fprintf(stderr, "dlopen probe: no symbol %s\n", name);
    missing = 1;
    return;
  }
  /* ISO C has no conversion from an object pointer to a function pointer;
   * POSIX guarantees the representation is the same. */
  memcpy(out, &symbol, size);
}

#define FIND(handle, api, field, name) \
  find(handle, PROBE_STR(name), &(api).field, sizeof (api).field)

int main(int argc, char ** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: probe <library.so> <module.so>\n");
    return 2;
  }
  void * library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (library == NULL) {
    fprintf(stderr, "dlopen probe: %s\n", dlerror());
    return 1;
  }
  ProbeApi api;
  FIND(library, api, group_create, grcore_group_create);
  FIND(library, api, group_destroy, grcore_group_destroy);
  FIND(library, api, context_create, grcore_context_create);
  FIND(library, api, context_destroy, grcore_context_destroy);
  FIND(library, api, context_is_owner, grcore_context_is_owner);
  FIND(library, api, engine_register, grcore_engine_register);
  FIND(library, api, context_stack, grcore_context_stack);
  FIND(library, api, activation_enter, grcore_activation_enter);
  FIND(library, api, activation_leave, grcore_activation_leave);
  FIND(library, api, activation_count, grcore_activation_count);
  if (missing) {
    return 1;
  }
  int failed = probe_exercise(&api, "dlopen of the library");
  /* Closed first, so that the module's dependency is a load of its own. */
  dlclose(library);

  void * module = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  if (module == NULL) {
    fprintf(stderr, "dlopen probe: %s\n", dlerror());
    return 1;
  }
  int (*run)(void) = NULL;
  find(module, "dlopen_probe_module_run", &run, sizeof run);
  if (missing) {
    return 1;
  }
  failed |= run();
  dlclose(module);
  if (failed) {
    return 1;
  }
  printf("check-dlopen: a library loaded by dlopen, and one loaded as a "
         "module's dependency, each entered and left records on two threads\n");
  return 0;
}
