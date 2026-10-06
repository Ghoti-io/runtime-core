/**
 * @file
 *
 * A runaway call is stopped at its own boundary, and the page around it
 * finishes (AD-21).
 *
 * A page is rendered from three panes: a sidebar, a nav pane and a footer. The
 * nav pane has a bug and loops forever. Without budget scopes the only limit
 * is the request's, so the nav pane would spend the whole page's allowance and
 * the page would fail. With them, each pane is a *template call* that opens a
 * budget scope on entry: the nav pane runs out of its own budget, the engine
 * unwinds the guest stack to the pane's boundary and closes the scope, and the
 * page goes on to render the footer with its allowance intact.
 *
 * There is no real engine here. A toy descriptor stands in, and the guest is
 * a few C functions that keep their frames on the guest stack. What the
 * example shows is the cycle every engine follows: open a scope at a call
 * boundary, charge fuel and poll, and on an unwind that names the scope
 * (`grcore_budget_scope_exhausted`), `grcore_budget_scope_unwind` and carry on.
 *
 * Build and run with `make examples`.
 */

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdint.h>
#include <stdio.h>

/* Checked in every build: an example that asserts nothing under NDEBUG is
 * not an example of anything. */
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "scoped_pages: check failed at line %d: %s\n", __LINE__, \
          #condition);                                                         \
      return 1;                                                                \
    }                                                                          \
  } while (0)

/* Each frame is one call and has one slot. The toy engine counts the frames
 * the unwinder releases, which is how the example shows that a cut-off pane's
 * frames were taken off the stack and not just forgotten. */
static unsigned frames_released;

static void toy_unwind(
    GRCORE_Context * context, const GRCORE_AbstractFrame * frame) {
  (void)context;
  (void)frame;
  frames_released++;
}

static const GRCORE_EngineDescriptor toy_engine = GRCORE_ENGINE_DESCRIPTOR_INIT("toy", NULL, NULL, NULL,
    {NULL, NULL, NULL}, {0, 0, 0}, NULL, toy_unwind, NULL, NULL);

typedef struct {
  const char * name;
  uint64_t budget;     /* the pane's own fuel budget */
  uint64_t cost;       /* fuel per step */
  unsigned steps;      /* 0: loops forever */
  int cut_off;         /* out: stopped at its boundary */
  uint64_t used;       /* out: fuel the pane used itself */
  unsigned iterations; /* out: steps run */
} Pane;

typedef struct {
  GRCORE_EngineId engine;
  Pane panes[3];
  uint64_t page_used;
  uint64_t page_remaining;
  int failed;
} Page;

/* One template call. The frame is the call; the scope is its budget. Each
 * step calls one level deeper (a template that recurses), charges, and polls
 * at a named site. */
static int render_pane(GRCORE_Context * context, Page * page, Pane * pane) {
  GRCORE_Stack * stack = grcore_context_stack(context);
  GRCORE_FrameRef frame;
  GRCORE_BudgetScope scope;
  if (grcore_stack_push(stack, page->engine, 1, &frame) != GRCORE_OK ||
      grcore_budget_scope_open(
          stack, pane->budget, GRCORE_SCOPE_POLICY_UNWIND, &scope) != GRCORE_OK) {
    return -1;
  }
  for (;;) {
    if (pane->steps != 0 && pane->iterations == pane->steps) {
      break; /* the template ended on its own */
    }
    GRCORE_FrameRef deeper;
    if (grcore_stack_push(stack, page->engine, 1, &deeper) != GRCORE_OK) {
      return -1;
    }
    grcore_context_charge_fuel(context, pane->cost);
    pane->iterations++;
    GRCORE_Verdict verdict = grcore_stack_poll(context, 1, pane->iterations);
    if (verdict == GRCORE_VERDICT_UNWIND) {
      /* The poll unwound. If this pane's scope is the cause, stop here and
       * only here; if not (terminate, say), the whole run is over. */
      if (!grcore_budget_scope_exhausted(stack, scope)) {
        return -1;
      }
      pane->used = 0;
      grcore_context_fuel_scope_used(context, scope.id, &pane->used);
      size_t popped;
      if (grcore_budget_scope_unwind(stack, scope, &popped) != GRCORE_OK ||
          popped != pane->iterations) { /* one level per step */
        return -1;
      }
      pane->cut_off = 1;
      grcore_stack_pop(stack); /* the pane's own call frame */
      return 0;
    }
    if (verdict == GRCORE_VERDICT_PAUSE) {
      return -1; /* nothing in this example asks for a pause */
    }
  }
  /* The template returned: its levels come off one by one. */
  for (unsigned level = 0; level < pane->iterations; level++) {
    grcore_stack_pop(stack);
  }
  grcore_context_fuel_scope_used(context, scope.id, &pane->used);
  if (grcore_budget_scope_close(stack, scope) != GRCORE_OK) {
    return -1;
  }
  grcore_stack_pop(stack);
  return 0;
}

static GRCORE_Step page_entry(GRCORE_Context * context, void * state) {
  Page * page = state;
  GRCORE_Stack * stack = grcore_context_stack(context);
  GRCORE_FrameRef frame;
  GRCORE_BudgetScope page_scope;
  if (grcore_stack_push(stack, page->engine, 1, &frame) != GRCORE_OK ||
      grcore_budget_scope_open(
          stack, 1000, GRCORE_SCOPE_POLICY_UNWIND, &page_scope) != GRCORE_OK) {
    page->failed = 1;
    return GRCORE_STEP_FINISHED;
  }
  grcore_context_charge_fuel(context, 10); /* the page's own work, before */
  for (int i = 0; i < 3; i++) {
    if (render_pane(context, page, &page->panes[i]) != 0) {
      page->failed = 1;
      return GRCORE_STEP_FINISHED;
    }
  }
  grcore_context_charge_fuel(context, 20); /* and after */
  grcore_context_fuel_scope_used(context, page_scope.id, &page->page_used);
  grcore_context_fuel_scope_remaining(
      context, page_scope.id, &page->page_remaining);
  if (grcore_budget_scope_close(stack, page_scope) != GRCORE_OK) {
    page->failed = 1;
  }
  grcore_stack_pop(stack);
  return GRCORE_STEP_FINISHED;
}

int main(void) {
  GRCORE_Options * options;
  GRCORE_Group * group;
  GRCORE_Context * context;
  CHECK(grcore_options_create(NULL, &options) == GRCORE_OK);
  /* The request's ceiling: the absolute limit, inclusive of every scope. */
  CHECK(grcore_options_set_fuel(options, 100000) == GRCORE_OK);
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
  CHECK(grcore_context_create(group, options, &context) == GRCORE_OK);
  grcore_options_destroy(options);

  Page page = {0, {{"sidebar", 200, 10, 5, 0, 0, 0},
                     {"nav", 100, 7, 0, 0, 0, 0},
                     {"footer", 200, 10, 4, 0, 0, 0}},
      0, 0, 0};
  CHECK(grcore_engine_register(context, &toy_engine, &page.engine) == GRCORE_OK);

  GRCORE_Outcome outcome;
  CHECK(grcore_run(context, page_entry, &page, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_FINISHED);
  CHECK(!page.failed);

  for (int i = 0; i < 3; i++) {
    printf("%-8s %s after %2u steps, used %3llu of %3llu\n", page.panes[i].name,
        page.panes[i].cut_off ? "cut off" : "done   ", page.panes[i].iterations,
        (unsigned long long)page.panes[i].used,
        (unsigned long long)page.panes[i].budget);
  }
  printf("page     used %llu of 1000 itself; the request spent %llu of 100000\n",
      (unsigned long long)page.page_used,
      (unsigned long long)grcore_context_fuel_used(context));

  /* The nav pane ran out of its own 100 on its 15th step and was stopped. */
  CHECK(page.panes[1].cut_off);
  CHECK(page.panes[1].iterations == 15);
  CHECK(page.panes[1].used == 105);
  /* The sidebar and the footer finished, each with its whole budget to spend:
   * the footer was not starved by the nav pane. */
  CHECK(!page.panes[0].cut_off && page.panes[0].used == 50);
  CHECK(!page.panes[2].cut_off && page.panes[2].used == 40);
  /* The page's clock stopped while each pane ran: only its own 30 counts. */
  CHECK(page.page_used == 30);
  CHECK(page.page_remaining == 970);
  /* The ceiling counted everything: 30 + 50 + 105 + 40. */
  CHECK(grcore_context_fuel_used(context) == 225);
  /* The nav pane's 15 levels were released by the unwinder (the engine's hook
   * saw each, innermost first), and nothing is left open. */
  CHECK(frames_released == 15);
  CHECK(grcore_stack_frame_count(grcore_context_stack(context)) == 0);
  CHECK(grcore_context_fuel_scope_depth(context) == 0);
  CHECK(grcore_context_depth(context, GRCORE_DEPTH_GUEST) == 0);

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return 0;
}
