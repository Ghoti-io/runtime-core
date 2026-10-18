#include "test_helpers.h"

namespace {
struct UnwindObservation {
  GRCORE_Result result=GRCORE_ERR_INTERNAL;
  GRCORE_EngineId engine=0;
  uint64_t identity=0;
};
UnwindObservation *g_unwind_observation=nullptr;

void observe_boundary_exception(GRCORE_Context *context,
    const GRCORE_AbstractFrame *frame){
  if(g_unwind_observation==nullptr||frame->slot_count<1)return;
  uint64_t raw=0;
  if(grcore_stack_slot_get(grcore_context_stack(context),frame->frame,0,&raw)
      !=GRCORE_OK)return;
  GRCORE_BoundaryRef boundary{raw};
  g_unwind_observation->result=grcore_boundary_get_foreign_exception(
      grcore_context_stack(context),boundary,&g_unwind_observation->engine,
      &g_unwind_observation->identity);
}
const GRCORE_EngineDescriptor kBoundaryUnwindObserver =
    GRCORE_ENGINE_DESCRIPTOR_INIT("boundary-unwind-observer",nullptr,nullptr,
        nullptr,GRCORE_ScopeInterface{nullptr,nullptr,nullptr},
        GRCORE_ConservativeDecoder{0,0,0},nullptr,observe_boundary_exception,
        nullptr,nullptr);

struct BoundaryRun {
  GRCORE_Stack *stack;
  GRCORE_EngineId caller;
  GRCORE_EngineId callee;
  GRCORE_BoundaryRef ref{0};
  bool entered=false;
  bool resumable=true;
};
GRCORE_Step boundary_entry(GRCORE_Context *context,void *opaque){
  auto *state=static_cast<BoundaryRun *>(opaque);
  if(!state->entered){
    if(grcore_boundary_enter(state->stack,state->caller,state->callee,
        (uintptr_t)0x1234,state->resumable,&state->ref)!=GRCORE_OK)
      return GRCORE_STEP_UNWOUND;
    state->entered=true;
  }
  GRCORE_Verdict verdict=grcore_poll(context,GRCORE_HERE);
  if(verdict==GRCORE_VERDICT_PAUSE)return GRCORE_STEP_PAUSED;
  if(verdict==GRCORE_VERDICT_UNWIND){
    (void)grcore_boundary_leave(state->stack,state->ref);
    return GRCORE_STEP_UNWOUND;
  }
  if(grcore_boundary_leave(state->stack,state->ref)!=GRCORE_OK)
    return GRCORE_STEP_UNWOUND;
  return GRCORE_STEP_FINISHED;
}
}

TEST(Boundary, ActiveInstanceAndForeignExceptionIdentitySurvivePauseAndResume){
  StackWorld world;
  GRCORE_Port *port=nullptr;
  ASSERT_EQ(grcore_context_port(world.ctx,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(port);
  BoundaryRun state{world.stack,world.alpha,world.beta};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(world.ctx,boundary_entry,&state,&outcome),GRCORE_OK);
  ASSERT_EQ(outcome,GRCORE_OUTCOME_PAUSED);
  uintptr_t instance=0;
  ASSERT_EQ(grcore_boundary_active_instance(world.stack,world.beta,&instance),GRCORE_OK);
  EXPECT_EQ(instance,(uintptr_t)0x1234);
  constexpr uint64_t exception_identity=UINT64_C(0xdeadbeef12345678);
  ASSERT_EQ(grcore_boundary_set_foreign_exception(world.stack,state.ref,
      world.alpha,exception_identity),GRCORE_OK);
  GRCORE_EngineId origin=0;uint64_t identity=0;
  ASSERT_EQ(grcore_boundary_get_foreign_exception(world.stack,state.ref,
      &origin,&identity),GRCORE_OK);
  EXPECT_EQ(origin,world.alpha);EXPECT_EQ(identity,exception_identity);
  ASSERT_EQ(grcore_resume(world.ctx,&outcome),GRCORE_OK);
  EXPECT_EQ(outcome,GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(grcore_activation_count(world.stack),0u);
}

TEST(Boundary, OrdinaryNestedActivationStillLimitUnwindsInsteadOfPausing){
  StackWorld world;
  GRCORE_Port *port=nullptr;
  ASSERT_EQ(grcore_context_port(world.ctx,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(port);
  BoundaryRun state{world.stack,world.alpha,world.beta,{0},false,false};
  GRCORE_Outcome outcome;
  EXPECT_EQ(grcore_run(world.ctx,boundary_entry,&state,&outcome),GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_activation_count(world.stack),0u);
}

GRCORE_Step boundary_then_poll_in_native(GRCORE_Context *context,void *opaque){
  auto *world=static_cast<StackWorld *>(opaque);
  GRCORE_ActivationRef native{0};
  if(grcore_activation_enter(world->stack,GRCORE_ACTIVATION_NATIVE,
      world->alpha,true,nullptr,&native)!=GRCORE_OK)
    return GRCORE_STEP_UNWOUND;
  GRCORE_BoundaryRef boundary{0};
  if(grcore_boundary_enter(world->stack,world->alpha,world->beta,
      (uintptr_t)0x6789,true,&boundary)!=GRCORE_OK){
    (void)grcore_activation_leave(world->stack,native);
    return GRCORE_STEP_UNWOUND;
  }
  if(grcore_boundary_leave(world->stack,boundary)!=GRCORE_OK){
    (void)grcore_activation_leave(world->stack,native);
    return GRCORE_STEP_UNWOUND;
  }
  GRCORE_Verdict verdict=grcore_poll(context,GRCORE_HERE);
  (void)grcore_activation_leave(world->stack,native);
  return verdict==GRCORE_VERDICT_PAUSE?GRCORE_STEP_PAUSED:
      verdict==GRCORE_VERDICT_UNWIND?GRCORE_STEP_UNWOUND:
      GRCORE_STEP_FINISHED;
}

GRCORE_Step unwind_boundary_then_poll_in_native(GRCORE_Context *context,
    void *opaque){
  auto *world=static_cast<StackWorld *>(opaque);
  GRCORE_ActivationRef native{0};
  if(grcore_activation_enter(world->stack,GRCORE_ACTIVATION_NATIVE,
      world->alpha,true,nullptr,&native)!=GRCORE_OK)
    return GRCORE_STEP_UNWOUND;
  GRCORE_BoundaryRef boundary{0};
  if(grcore_boundary_enter(world->stack,world->alpha,world->beta,
      (uintptr_t)0x6789,true,&boundary)!=GRCORE_OK){
    (void)grcore_activation_leave(world->stack,native);
    return GRCORE_STEP_UNWOUND;
  }
  size_t popped=0;
  if(grcore_unwind_to_activation(world->stack,native,&popped)!=GRCORE_OK||
      grcore_activation_count(world->stack)!=1){
    (void)grcore_activation_leave(world->stack,native);
    return GRCORE_STEP_UNWOUND;
  }
  GRCORE_Verdict verdict=grcore_poll(context,GRCORE_HERE);
  (void)grcore_activation_leave(world->stack,native);
  return verdict==GRCORE_VERDICT_PAUSE?GRCORE_STEP_PAUSED:
      verdict==GRCORE_VERDICT_UNWIND?GRCORE_STEP_UNWOUND:
      GRCORE_STEP_FINISHED;
}

TEST(Boundary, LeavingResumableBoundaryRestoresNestedPauseDenial){
  StackWorld world;
  GRCORE_Port *port=nullptr;
  ASSERT_EQ(grcore_context_port(world.ctx,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(port);
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  EXPECT_EQ(grcore_run(world.ctx,boundary_then_poll_in_native,&world,&outcome),
      GRCORE_ERR_LIMIT);
  EXPECT_NE(outcome,GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(grcore_activation_count(world.stack),0u);
}

TEST(Boundary, UnwindingResumableBoundaryRestoresNestedPauseDenial){
  StackWorld world;
  GRCORE_Port *port=nullptr;
  ASSERT_EQ(grcore_context_port(world.ctx,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(port);
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  EXPECT_EQ(grcore_run(world.ctx,unwind_boundary_then_poll_in_native,&world,
      &outcome),GRCORE_ERR_LIMIT);
  EXPECT_NE(outcome,GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(grcore_activation_count(world.stack),0u);
}

TEST(Boundary, RejectsZeroCallerEngine){
  StackWorld world;
  GRCORE_BoundaryRef boundary{0};
  EXPECT_EQ(grcore_boundary_enter(world.stack,0,world.beta,(uintptr_t)0x1234,
      true,&boundary),GRCORE_ERR_INVALID);
  EXPECT_EQ(boundary.id,0u);
}

TEST(Boundary, ForeignExceptionIdentitySurvivesUntilUnwindHookRuns){
  StackWorld world;
  GRCORE_EngineId observer=0;
  ASSERT_EQ(grcore_engine_register(world.ctx,&kBoundaryUnwindObserver,
      &observer),GRCORE_OK);
  GRCORE_ActivationRef host{0};
  ASSERT_EQ(grcore_activation_enter(world.stack,GRCORE_ACTIVATION_HOST,0,
      false,nullptr,&host),GRCORE_OK);
  GRCORE_BoundaryRef boundary{0};
  ASSERT_EQ(grcore_boundary_enter(world.stack,world.alpha,world.beta,
      (uintptr_t)0x4567,false,&boundary),GRCORE_OK);
  constexpr uint64_t exception_identity=UINT64_C(0x7f01a11ce55);
  ASSERT_EQ(grcore_boundary_set_foreign_exception(world.stack,boundary,
      world.alpha,exception_identity),GRCORE_OK);
  GRCORE_FrameRef frame{0};
  ASSERT_EQ(grcore_stack_push(world.stack,observer,1,&frame),GRCORE_OK);
  ASSERT_EQ(grcore_stack_slot_set(world.stack,frame,0,boundary.id),GRCORE_OK);

  UnwindObservation observed;
  g_unwind_observation=&observed;
  size_t popped=0;
  GRCORE_Result unwind=grcore_unwind_to_activation(world.stack,host,&popped);
  g_unwind_observation=nullptr;
  ASSERT_EQ(unwind,GRCORE_OK);
  EXPECT_EQ(popped,1u);
  EXPECT_EQ(observed.result,GRCORE_OK);
  EXPECT_EQ(observed.engine,world.alpha);
  EXPECT_EQ(observed.identity,exception_identity);
  EXPECT_EQ(grcore_activation_count(world.stack),1u);
  EXPECT_EQ(grcore_stack_frame_count(world.stack),0u);
  GRCORE_EngineId expired_engine=0;uint64_t expired_identity=0;
  EXPECT_EQ(grcore_boundary_get_foreign_exception(world.stack,boundary,
      &expired_engine,&expired_identity),GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_leave(world.stack,host),GRCORE_OK);
  EXPECT_EQ(grcore_activation_count(world.stack),0u);
}

int main(int argc,char**argv){
  ::testing::InitGoogleTest(&argc,argv);
  return RUN_ALL_TESTS();
}
