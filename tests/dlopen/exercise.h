/**
 * @file
 *
 * What the dlopen probe does with the library once it has it: through a table
 * of entry points, so that the probe (which finds them with dlsym) and the
 * module (which is linked against the library) run the same sequence.
 *
 * On the thread that calls it and on a thread started afterwards, each with
 * its own context: an activation record entered and left, the owner check
 * true; and from the second thread, the first thread's stack refused. The
 * thread id the owner check reads is the library's one thread-local, so this
 * is what a dlopen that placed it in static TLS, or one that could not, must
 * still get right.
 */

#ifndef GHOTI_IO_GRCORE_TESTS_DLOPEN_EXERCISE_H
#define GHOTI_IO_GRCORE_TESTS_DLOPEN_EXERCISE_H

#include <ghoti.io/runtime-core/runtime-core.h>

#include <pthread.h>
#include <stdio.h>

typedef struct ProbeApi {
  __typeof__(grcore_group_create) * group_create;
  __typeof__(grcore_group_destroy) * group_destroy;
  __typeof__(grcore_context_create) * context_create;
  __typeof__(grcore_context_destroy) * context_destroy;
  __typeof__(grcore_context_is_owner) * context_is_owner;
  __typeof__(grcore_engine_register) * engine_register;
  __typeof__(grcore_context_stack) * context_stack;
  __typeof__(grcore_activation_enter) * activation_enter;
  __typeof__(grcore_activation_leave) * activation_leave;
  __typeof__(grcore_activation_count) * activation_count;
} ProbeApi;

static const GRCORE_EngineDescriptor probe_engine = GRCORE_ENGINE_DESCRIPTOR_INIT(
    "dlopen-probe", NULL, NULL, NULL, {NULL, NULL, NULL}, {0, 0, 0}, NULL,
    NULL, NULL, NULL);

typedef struct ProbeRun {
  const ProbeApi * api;
  const char * where;
  GRCORE_Stack * foreign; ///< Another thread's stack, which must be refused.
  int failed;
} ProbeRun;

#define PROBE_EXPECT(run, cond)                                              \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "dlopen probe (%s): %s:%d: %s\n", (run)->where,       \
          __FILE__, __LINE__, #cond);                                        \
      (run)->failed = 1;                                                     \
    }                                                                        \
  } while (0)

static void * probe_thread(void * arg) {
  ProbeRun * run = arg;
  const ProbeApi * a = run->api;
  if (run->foreign != NULL) {
    GRCORE_ActivationRef ref = {0};
    PROBE_EXPECT(run, a->activation_enter(run->foreign, GRCORE_ACTIVATION_JIT,
                          0, false, NULL, &ref) == GRCORE_ERR_INVALID);
  }
  GRCORE_Group * group = NULL;
  GRCORE_Context * context = NULL;
  GRCORE_EngineId engine = 0;
  if (a->group_create(NULL, NULL, &group) != GRCORE_OK ||
      a->context_create(group, NULL, &context) != GRCORE_OK ||
      a->engine_register(context, &probe_engine, &engine) != GRCORE_OK) {
    PROBE_EXPECT(run, !"setup");
    return NULL;
  }
  GRCORE_Stack * stack = a->context_stack(context);
  PROBE_EXPECT(run, a->context_is_owner(context));
  for (int i = 0; i < 1000; i++) {
    GRCORE_ActivationRef ref = {0};
    PROBE_EXPECT(run, a->activation_enter(stack, GRCORE_ACTIVATION_JIT, engine,
                          false, NULL, &ref) == GRCORE_OK);
    PROBE_EXPECT(run, a->activation_count(stack) == 1);
    PROBE_EXPECT(run, a->activation_leave(stack, ref) == GRCORE_OK);
  }
  PROBE_EXPECT(run, a->activation_count(stack) == 0);
  PROBE_EXPECT(run, a->context_destroy(context) == GRCORE_OK);
  PROBE_EXPECT(run, a->group_destroy(group) == GRCORE_OK);
  return NULL;
}

/* Runs the sequence here and on a new thread; returns 0 when all held. */
static int probe_exercise(const ProbeApi * api, const char * where) {
  ProbeRun here = {api, where, NULL, 0};
  probe_thread(&here);
  /* A context of this thread's, entered while the second thread tries it. */
  GRCORE_Group * group = NULL;
  GRCORE_Context * context = NULL;
  GRCORE_EngineId engine = 0;
  if (api->group_create(NULL, NULL, &group) != GRCORE_OK ||
      api->context_create(group, NULL, &context) != GRCORE_OK ||
      api->engine_register(context, &probe_engine, &engine) != GRCORE_OK) {
    fprintf(stderr, "dlopen probe (%s): setup failed\n", where);
    return 1;
  }
  ProbeRun there = {api, where, api->context_stack(context), 0};
  pthread_t thread;
  if (pthread_create(&thread, NULL, probe_thread, &there) != 0 ||
      pthread_join(thread, NULL) != 0) {
    fprintf(stderr, "dlopen probe (%s): no thread\n", where);
    return 1;
  }
  PROBE_EXPECT(&here, api->activation_count(there.foreign) == 0);
  PROBE_EXPECT(&here, api->context_destroy(context) == GRCORE_OK);
  PROBE_EXPECT(&here, api->group_destroy(group) == GRCORE_OK);
  return here.failed || there.failed;
}

#endif
