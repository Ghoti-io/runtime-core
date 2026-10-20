/**
 * @file
 *
 * A module linked against the library, as an engine plugin is: the dlopen
 * probe loads this, which brings the library in as its dependency, and calls
 * dlopen_probe_module_run.
 */

#include "exercise.h"

__attribute__((visibility("default"))) int dlopen_probe_module_run(void);

int dlopen_probe_module_run(void) {
  static const ProbeApi api = {
      grcore_group_create,
      grcore_group_destroy,
      grcore_context_create,
      grcore_context_destroy,
      grcore_context_is_owner,
      grcore_engine_register,
      grcore_context_stack,
      grcore_activation_enter,
      grcore_activation_leave,
      grcore_activation_count,
  };
  return probe_exercise(&api, "through a module");
}
