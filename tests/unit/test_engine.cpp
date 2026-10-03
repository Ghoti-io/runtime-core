/**
 * @file
 *
 * Engine descriptors: registration, lookup, inspection and the decoder.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/a/guest_internal.h"

#include <cstring>
#include <thread>

TEST(Engine, RegisterAssignsIdsFromOneAndCreatesTheStack) {
  RunWorld w;
  EXPECT_EQ(grcore_context_stack(w.ctx), nullptr);
  EXPECT_EQ(grcore_engine_count(w.ctx), 0u);
  GRCORE_EngineId a = 0;
  GRCORE_EngineId b = 0;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &a), GRCORE_OK);
  EXPECT_EQ(a, 1u);
  GRCORE_Stack * stack = grcore_context_stack(w.ctx);
  ASSERT_NE(stack, nullptr);
  ASSERT_EQ(grcore_engine_register(w.ctx, &kBeta, &b), GRCORE_OK);
  EXPECT_EQ(b, 2u);
  EXPECT_EQ(grcore_context_stack(w.ctx), stack); // one stack per context
  EXPECT_EQ(grcore_engine_count(w.ctx), 2u);
  EXPECT_EQ(grcore_engine_descriptor(w.ctx, a), &kAlpha);
  EXPECT_EQ(grcore_engine_descriptor(w.ctx, b), &kBeta);
}

TEST(Engine, TheStackIsPerContextStateRegisteredUnderACardinalityOneKey) {
  RunWorld w;
  GRCORE_EngineId id;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &id), GRCORE_OK);
  EXPECT_EQ(grcore_context_slot(w.ctx, &grcore_guest_key),
      static_cast<void *>(grcore_context_stack(w.ctx)));
  EXPECT_EQ(grcore_guest_key.cardinality, GRCORE_CARDINALITY_ONE);
  EXPECT_EQ(grcore_guest_key.phase, GRCORE_PHASE_NONE);
  // A second engine does not register a second state.
  size_t before = grcore_context_registration_count(w.ctx);
  ASSERT_EQ(grcore_engine_register(w.ctx, &kBeta, &id), GRCORE_OK);
  EXPECT_EQ(grcore_context_registration_count(w.ctx), before);
  // And the key refuses a second, as any cardinality-one key does.
  int dummy = 0;
  EXPECT_EQ(grcore_context_register(w.ctx, &grcore_guest_key, &dummy),
      GRCORE_ERR_INVALID);
}

TEST(Engine, LookupOfNothingIsNull) {
  RunWorld w;
  EXPECT_EQ(grcore_engine_descriptor(w.ctx, 0), nullptr);
  EXPECT_EQ(grcore_engine_descriptor(w.ctx, 1), nullptr);
  EXPECT_EQ(grcore_engine_descriptor(nullptr, 1), nullptr);
  EXPECT_EQ(grcore_engine_count(nullptr), 0u);
  GRCORE_EngineId id;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &id), GRCORE_OK);
  EXPECT_EQ(grcore_engine_descriptor(w.ctx, 0), nullptr);
  EXPECT_EQ(grcore_engine_descriptor(w.ctx, 2), nullptr);
}

TEST(Engine, RegisterRefusesBadArgumentsAndLeavesTheTableUnchanged) {
  RunWorld w;
  GRCORE_EngineId id = 77;
  EXPECT_EQ(grcore_engine_register(nullptr, &kAlpha, &id), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_engine_register(w.ctx, nullptr, &id), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_engine_register(w.ctx, &kAlpha, nullptr), GRCORE_ERR_INVALID);
  static const GRCORE_EngineDescriptor no_name = {nullptr, nullptr, nullptr,
      nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
      GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr};
  static const GRCORE_EngineDescriptor empty_name = {"", nullptr, nullptr,
      nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
      GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr};
  EXPECT_EQ(grcore_engine_register(w.ctx, &no_name, &id), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_engine_register(w.ctx, &empty_name, &id), GRCORE_ERR_INVALID);
  EXPECT_EQ(id, 77u); // never written on a refusal
  EXPECT_EQ(grcore_engine_count(w.ctx), 0u);
  EXPECT_EQ(grcore_context_stack(w.ctx), nullptr); // not even the state
  EXPECT_EQ(grcore_context_registration_count(w.ctx), 0u);
}

TEST(Engine, RegisteringTheSameDescriptorTwiceIsRefused) {
  RunWorld w;
  GRCORE_EngineId first = 0;
  GRCORE_EngineId again = 55;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &first), GRCORE_OK);
  EXPECT_EQ(grcore_engine_register(w.ctx, &kAlpha, &again), GRCORE_ERR_INVALID);
  EXPECT_EQ(again, 55u);
  EXPECT_EQ(grcore_engine_count(w.ctx), 1u);
}

TEST(Engine, TheSameDescriptorMayServeTwoContexts) {
  RunWorld a;
  RunWorld b;
  GRCORE_EngineId ia, ib;
  EXPECT_EQ(grcore_engine_register(a.ctx, &kAlpha, &ia), GRCORE_OK);
  EXPECT_EQ(grcore_engine_register(b.ctx, &kAlpha, &ib), GRCORE_OK);
  EXPECT_NE(grcore_context_stack(a.ctx), grcore_context_stack(b.ctx));
}

TEST(Engine, RegisterIsRefusedWhileRunningAndAllowedAgainWhenPaused) {
  RunWorld w(0);
  GRCORE_Result during = GRCORE_OK;
  GRCORE_EngineId id = 9;
  Fn fn{[&](GRCORE_Context * c) {
    during = grcore_engine_register(c, &kAlpha, &id);
    grcore_context_charge_fuel(c, 1);
    return GRCORE_POLL(c) == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED
                                                  : GRCORE_STEP_FINISHED;
  }};
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(during, GRCORE_ERR_INVALID);
  EXPECT_EQ(id, 9u);
  EXPECT_EQ(grcore_engine_count(w.ctx), 0u);
  // Paused is a state the table can change in.
  EXPECT_EQ(grcore_engine_register(w.ctx, &kAlpha, &id), GRCORE_OK);
  EXPECT_EQ(grcore_engine_count(w.ctx), 1u);
}

TEST(Engine, RegisterFromAThreadThatDoesNotOwnTheContextIsRefused) {
  RunWorld w;
  GRCORE_Result other = GRCORE_OK;
  GRCORE_EngineId id = 4;
  std::thread([&] { other = grcore_engine_register(w.ctx, &kAlpha, &id); }).join();
  EXPECT_EQ(other, GRCORE_ERR_INVALID);
  EXPECT_EQ(id, 4u);
  EXPECT_EQ(grcore_context_stack(w.ctx), nullptr);
}

TEST(Engine, ManyEnginesGrowTheTableAndKeepEveryDescriptor) {
  RunWorld w;
  static GRCORE_EngineDescriptor many[40];
  static char names[40][16];
  for (int i = 0; i < 40; i++) {
    std::snprintf(names[i], sizeof names[i], "e%d", i);
    many[i] = kGamma;
    many[i].name = names[i];
    GRCORE_EngineId id;
    ASSERT_EQ(grcore_engine_register(w.ctx, &many[i], &id), GRCORE_OK);
    ASSERT_EQ(id, static_cast<GRCORE_EngineId>(i + 1));
  }
  for (int i = 0; i < 40; i++) {
    EXPECT_EQ(grcore_engine_descriptor(w.ctx, static_cast<GRCORE_EngineId>(i + 1)),
        &many[i]);
  }
}

TEST(Engine, EveryAllocationFailureOfRegisterIsOomAndLeaksNothing) {
  bool succeeded = false;
  int failures = 0;
  for (long n = 1; n < 50 && !succeeded; n++) {
    TrackingAllocator t;
    {
      RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          GRCORE_DEFAULT_MEMORY_RESERVE, GRCORE_UNLIMITED, GRCORE_UNLIMITED,
          t.get());
      t.fail_at = t.calls + n;
      GRCORE_EngineId id = 31;
      GRCORE_Result r = grcore_engine_register(w.ctx, &kAlpha, &id);
      t.fail_at = 0;
      if (r == GRCORE_OK) {
        succeeded = true;
        EXPECT_EQ(id, 1u);
      } else {
        failures++;
        EXPECT_EQ(r, GRCORE_ERR_OOM) << "n=" << n;
        EXPECT_EQ(id, 31u) << "n=" << n;
        EXPECT_EQ(grcore_engine_count(w.ctx), 0u) << "n=" << n;
        EXPECT_EQ(grcore_context_stack(w.ctx), nullptr) << "n=" << n;
        // Nothing the failed attempt took is still charged to the context.
        EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 0u) << "n=" << n;
      }
    }
    EXPECT_EQ(t.live, 0) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  // The state, the engine table and the registration table at least.
  EXPECT_GE(failures, 3);
}

TEST(Engine, RegisterUnderATinyMemoryBudgetIsALimitAndLeavesTheTableUnchanged) {
  RunWorld w(GRCORE_UNLIMITED, 16, 0);
  GRCORE_EngineId id = 8;
  uint64_t refusals = grcore_context_memory_refusals(w.ctx);
  EXPECT_EQ(grcore_engine_register(w.ctx, &kAlpha, &id), GRCORE_ERR_LIMIT);
  EXPECT_GT(grcore_context_memory_refusals(w.ctx), refusals);
  EXPECT_EQ(id, 8u);
  EXPECT_EQ(grcore_engine_count(w.ctx), 0u);
  EXPECT_EQ(grcore_context_stack(w.ctx), nullptr);
  EXPECT_EQ(grcore_context_memory_blocks(w.ctx), 0u);
}

TEST(Engine, AFailedSecondRegistrationLeavesTheFirstWorking) {
  TrackingAllocator t;
  RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE,
      GRCORE_UNLIMITED, GRCORE_UNLIMITED, t.get());
  GRCORE_EngineId a;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &a), GRCORE_OK);
  // Fill the table so the next registration must grow it.
  static GRCORE_EngineDescriptor fill[3];
  for (auto & d : fill) {
    d = kGamma;
    d.name = "fill";
    GRCORE_EngineId id;
    ASSERT_EQ(grcore_engine_register(w.ctx, &d, &id), GRCORE_OK);
  }
  t.fail_at = t.calls + 1;
  GRCORE_EngineId id = 6;
  EXPECT_EQ(grcore_engine_register(w.ctx, &kBeta, &id), GRCORE_ERR_OOM);
  t.fail_at = 0;
  EXPECT_EQ(id, 6u);
  EXPECT_EQ(grcore_engine_count(w.ctx), 4u);
  EXPECT_EQ(grcore_engine_descriptor(w.ctx, a), &kAlpha);
  EXPECT_EQ(grcore_engine_register(w.ctx, &kBeta, &id), GRCORE_OK);
  EXPECT_EQ(id, 5u);
}

TEST(Engine, TheStateIsChargedToTheContextAndFreedAtDestroy) {
  TrackingAllocator t;
  {
    RunWorld w(GRCORE_UNLIMITED, GRCORE_UNLIMITED, GRCORE_DEFAULT_MEMORY_RESERVE,
        GRCORE_UNLIMITED, GRCORE_UNLIMITED, t.get());
    uint64_t before = grcore_context_memory_in_use(w.ctx);
    GRCORE_EngineId id;
    ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &id), GRCORE_OK);
    EXPECT_GT(grcore_context_memory_in_use(w.ctx), before);
    GRCORE_FrameRef f;
    ASSERT_EQ(grcore_stack_push(grcore_context_stack(w.ctx), id, 4, &f), GRCORE_OK);
    EXPECT_GE(grcore_context_memory_in_use(w.ctx),
        grcore_stack_bytes_capacity(grcore_context_stack(w.ctx)));
    // A frame is still on the stack: destroy must free it all the same.
  }
  EXPECT_EQ(t.live, 0);
}

TEST(Engine, DestroyRunsTheGuestKeyInReverseRegistrationOrder) {
  // The guest key was registered after this one, so it is destroyed first and
  // the other destructor may still read the context.
  static bool stack_gone_when_ours_ran = false;
  static const GRCORE_Key kEarly = {"early", GRCORE_CARDINALITY_ONE,
      GRCORE_PHASE_NONE,
      [](GRCORE_Context * c, void *) {
        stack_gone_when_ours_ran = grcore_context_stack(c) == nullptr;
      },
      nullptr};
  RunWorld w;
  int token = 0;
  ASSERT_EQ(grcore_context_register(w.ctx, &kEarly, &token), GRCORE_OK);
  GRCORE_EngineId id;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kAlpha, &id), GRCORE_OK);
  GRCORE_Context * c = w.ctx;
  w.ctx = nullptr;
  ASSERT_EQ(grcore_context_destroy(c), GRCORE_OK);
  EXPECT_TRUE(stack_gone_when_ours_ran);
}

TEST(Decoder, ComputesMaskShiftThenBase) {
  GRCORE_ConservativeDecoder d = {0xFF00, 8, 0x1000};
  uint64_t address = 0;
  ASSERT_TRUE(grcore_decoder_decode(&d, 0xABCD, &address));
  EXPECT_EQ(address, 0xABu + 0x1000u);
  // Bits outside the mask are ignored.
  ASSERT_TRUE(grcore_decoder_decode(&d, 0xFFFF00FFull << 8 | 0xFF, &address));
  EXPECT_EQ(address, 0xFFu + 0x1000u);
}

TEST(Decoder, AZeroMaskDecodesNothingAndWritesNothing) {
  GRCORE_ConservativeDecoder d = {0, 0, 0x1000};
  uint64_t address = 12345;
  EXPECT_FALSE(grcore_decoder_decode(&d, ~0ull, &address));
  EXPECT_EQ(address, 12345u);
}

TEST(Decoder, RefusesNullAndAShiftOfSixtyFourOrMore) {
  GRCORE_ConservativeDecoder d = {~0ull, 64, 0};
  uint64_t address = 7;
  EXPECT_FALSE(grcore_decoder_decode(nullptr, 1, &address));
  EXPECT_FALSE(grcore_decoder_decode(&d, 1, &address));
  d.shift = 63;
  EXPECT_FALSE(grcore_decoder_decode(&d, 1, nullptr));
  ASSERT_TRUE(grcore_decoder_decode(&d, ~0ull, &address));
  EXPECT_EQ(address, 1u);
}

TEST(Decoder, AnEngineDescriptorCarriesItsOwnDecoder) {
  StackWorld w;
  const GRCORE_EngineDescriptor * d = grcore_engine_descriptor(w.ctx, w.beta);
  uint64_t address = 0;
  ASSERT_TRUE(grcore_decoder_decode(&d->decoder, 0x12345, &address));
  EXPECT_EQ(address, ((0x12345ull & 0xFFFF0) >> 4) + 0x1000);
  EXPECT_FALSE(grcore_decoder_decode(
      &grcore_engine_descriptor(w.ctx, w.alpha)->decoder, 0x12345, &address));
}

TEST(Inspect, UsesTheEnginesInspectorWhenItHasOne) {
  StackWorld w;
  char buf[32];
  size_t length = 0;
  ASSERT_EQ(grcore_engine_inspect(w.ctx, w.alpha, GRCORE_SLOT_VALUE, 42, buf,
                sizeof buf, &length),
      GRCORE_OK);
  EXPECT_STREQ(buf, "a#42");
  EXPECT_EQ(length, 4u);
  ASSERT_EQ(grcore_engine_inspect(w.ctx, w.beta, GRCORE_SLOT_VALUE, 0xbeef, buf,
                sizeof buf, &length),
      GRCORE_OK);
  EXPECT_STREQ(buf, "b:beef");
}

TEST(Inspect, FallsBackToHexWithoutAnInspector) {
  RunWorld w;
  GRCORE_EngineId g;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kGamma, &g), GRCORE_OK);
  char buf[32];
  size_t length = 0;
  ASSERT_EQ(grcore_engine_inspect(w.ctx, g, GRCORE_SLOT_RAW, 255, buf, sizeof buf,
                &length),
      GRCORE_OK);
  EXPECT_STREQ(buf, "0xff");
  EXPECT_EQ(length, 4u);
}

TEST(Inspect, TruncatesToTheBufferAndReportsTheFullLength) {
  RunWorld w;
  GRCORE_EngineId g;
  ASSERT_EQ(grcore_engine_register(w.ctx, &kGamma, &g), GRCORE_OK);
  char buf[5];
  size_t length = 0;
  ASSERT_EQ(grcore_engine_inspect(w.ctx, g, GRCORE_SLOT_RAW, 0x123456, buf,
                sizeof buf, &length),
      GRCORE_OK);
  EXPECT_EQ(length, 8u);
  EXPECT_STREQ(buf, "0x12");
  // A size of zero is a length query and needs no buffer.
  length = 0;
  ASSERT_EQ(grcore_engine_inspect(w.ctx, g, GRCORE_SLOT_RAW, 0x123456, nullptr, 0,
                &length),
      GRCORE_OK);
  EXPECT_EQ(length, 8u);
}

TEST(Inspect, RefusesWhatItCannotRenderAndWritesNothing) {
  StackWorld w;
  char buf[8] = "keep";
  size_t length = 99;
  EXPECT_EQ(grcore_engine_inspect(w.ctx, 0, GRCORE_SLOT_RAW, 1, buf, 8, &length),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_engine_inspect(w.ctx, 9, GRCORE_SLOT_RAW, 1, buf, 8, &length),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_engine_inspect(nullptr, w.alpha, GRCORE_SLOT_RAW, 1, buf, 8,
                &length),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_engine_inspect(w.ctx, w.alpha, GRCORE_SLOT_RAW, 1, buf, 8,
                nullptr),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_engine_inspect(w.ctx, w.alpha, GRCORE_SLOT_RAW, 1, nullptr, 8,
                &length),
      GRCORE_ERR_INVALID);
  EXPECT_STREQ(buf, "keep");
  EXPECT_EQ(length, 99u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
