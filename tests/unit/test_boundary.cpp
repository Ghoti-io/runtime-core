#include "test_helpers.h"

namespace {
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

TEST(Boundary, RejectsZeroCallerEngine){
  StackWorld world;
  GRCORE_BoundaryRef boundary{0};
  EXPECT_EQ(grcore_boundary_enter(world.stack,0,world.beta,(uintptr_t)0x1234,
      true,&boundary),GRCORE_ERR_INVALID);
  EXPECT_EQ(boundary.id,0u);
}

int main(int argc,char**argv){
  ::testing::InitGoogleTest(&argc,argv);
  return RUN_ALL_TESTS();
}
