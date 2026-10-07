/**
 * @file
 *
 * The precise walk of compiled frames (AD-17, AD-28): the activation record's
 * compiled state, the walk over hand-built frames, the root source and the
 * abstract frame.
 *
 * The JIT does not emit calls yet, so every frame here is built by hand in an
 * array of words (`HandStack`) over code that is a byte region with a
 * hand-written stack map (`HandCode`), in the layout the contract says: the
 * base word holds the caller's base, the word after it the return address.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/a/guest_internal.h"

#include <algorithm>
#include <csignal>
#include <set>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

/* An address in no registered code: the entry from the interpreter. */
const char kOutsideBytes[64] = {};
const uintptr_t kOutside = reinterpret_cast<uintptr_t>(kOutsideBytes);

/* The code outlives the context that registered it (it is a base, so it is
 * destroyed last): a registry releases at destroy and the callback is the
 * fixture's. */
struct CodeHolder {
  HandCode code;
  CodeHolder() = default;
  CodeHolder(bool raw_live, bool dead_slot, bool derived)
      : code(raw_live, dead_slot, derived) {}
};
struct CWorld : CodeHolder, StackWorld {
  explicit CWorld(uint64_t fuel = GRCORE_UNLIMITED, bool raw_live = false,
      bool dead_slot = false, bool derived = false)
      : CodeHolder(raw_live, dead_slot, derived), StackWorld(fuel) {
    EXPECT_EQ(code.add_to(ctx, alpha), GRCORE_OK);
  }
};

GRCORE_ActivationRef enter(GRCORE_Stack * s, GRCORE_ActivationKind kind,
    const GRCORE_CSegment * seg = nullptr, GRCORE_EngineId engine = 0) {
  GRCORE_ActivationRef ref;
  EXPECT_EQ(grcore_activation_enter(s, kind, engine, false, seg, &ref), GRCORE_OK);
  return ref;
}

struct Walked {
  std::vector<GRCORE_CompiledFrame> frames;
  GRCORE_CompiledWalkStatus end = GRCORE_CWALK_END;
  std::string reason;
};

Walked walk_compiled(const GRCORE_Context * c) {
  Walked out;
  GRCORE_CompiledWalk walk;
  EXPECT_EQ(grcore_compiled_walk_begin(c, &walk), GRCORE_OK);
  GRCORE_CompiledFrame f;
  for (;;) {
    GRCORE_CompiledWalkStatus st = grcore_compiled_walk_next(&walk, &f);
    if (st == GRCORE_CWALK_FRAME) {
      out.frames.push_back(f);
      if (out.frames.size() > 100000) {
        ADD_FAILURE() << "the walk does not end";
        break;
      }
      continue;
    }
    out.end = st;
    if (st == GRCORE_CWALK_BROKEN) {
      const char * r = grcore_compiled_walk_reason(&walk);
      out.reason = r != nullptr ? r : "";
      // A broken walk stays broken, and says the same thing.
      EXPECT_EQ(grcore_compiled_walk_next(&walk, &f), GRCORE_CWALK_BROKEN);
      EXPECT_EQ(grcore_compiled_walk_next(&walk, &f), GRCORE_CWALK_BROKEN);
    }
    break;
  }
  return out;
}

struct Seen {
  std::vector<uint64_t *> slots;
  std::vector<uint64_t> values;
  std::vector<GRCORE_ConservativeRange> ranges;
  uint64_t rewrite_add = 0;
  bool take_slots = true;
  bool take_ranges = true;
};

GRCORE_RootVisitor visitor_for(Seen * seen) {
  GRCORE_RootVisitor v = {};
  v.user = seen;
  if (seen->take_slots) {
    v.slot = [](void * user, uint64_t * slot) {
      auto * s = static_cast<Seen *>(user);
      s->slots.push_back(slot);
      s->values.push_back(*slot);
      *slot += s->rewrite_add;
    };
  }
  if (seen->take_ranges) {
    v.range = [](void * user, const GRCORE_ConservativeRange * r) {
      static_cast<Seen *>(user)->ranges.push_back(*r);
    };
  }
  return v;
}

/* Runs `fn` in a forked child and says whether it ended in abort(), with what
 * it wrote to standard error. Root enumeration has no result to return, so
 * stopping the process is how it refuses a broken chain, and this is how a
 * test sees that. */
struct ChildResult {
  bool aborted = false;
  bool exited_clean = false;
  std::string err;
};
ChildResult in_child(const std::function<void()> & fn) {
#ifdef _WIN32
  (void)fn;
  ADD_FAILURE() << "no fork on this platform";
  return ChildResult{};
#else
  int fds[2];
  EXPECT_EQ(pipe(fds), 0);
  std::fflush(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], 2);
    fn();
    _exit(0);
  }
  close(fds[1]);
  ChildResult r;
  char buf[256];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0) {
    r.err.append(buf, static_cast<size_t>(n));
  }
  close(fds[0]);
  int status = 0;
  EXPECT_EQ(waitpid(pid, &status, 0), pid);
  r.aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
  r.exited_clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  return r;
#endif
}

/* Frames of a run, stopped at consecutive sites of the code, set on a JIT
 * record. */
struct ChainRun {
  std::vector<uintptr_t> bases;
  GRCORE_ActivationRef rec;
};
ChainRun set_up_run(CWorld & w, HandStack & st, size_t first_word, size_t n,
    size_t site0, const GRCORE_CSegment * seg = nullptr) {
  ChainRun r;
  r.bases = st.build(w.code, first_word, n, site0, kOutside);
  r.rec = enter(w.stack, GRCORE_ACTIVATION_JIT, seg);
  EXPECT_EQ(grcore_activation_set_compiled(
                w.stack, r.rec, r.bases[0], w.code.at(site0)),
      GRCORE_OK);
  return r;
}

} // namespace

/* ---- The record's compiled state ---------------------------------------- */

TEST(ActivationCompiled, AFreshRecordCarriesNoStateAndTheSetterWritesAndClearsIt) {
  CWorld w;
  GRCORE_ActivationRef rec = enter(w.stack, GRCORE_ACTIVATION_JIT);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, rec, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0u);
  EXPECT_EQ(info.return_address, 0u);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, 0x1000, 0x2000), GRCORE_OK);
  ASSERT_EQ(grcore_activation_info(w.stack, rec, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0x1000u);
  EXPECT_EQ(info.return_address, 0x2000u);
  ASSERT_EQ(grcore_activation_at(w.stack, 0, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0x1000u);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, 0, 0), GRCORE_OK);
  ASSERT_EQ(grcore_activation_info(w.stack, rec, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0u);
  EXPECT_EQ(info.return_address, 0u);
}

TEST(ActivationCompiled, OnlyAJitRecordMayCarryTheStateBecauseOnlyThoseKeepRetiredCodeAlive) {
  CWorld w;
  for (GRCORE_ActivationKind kind :
      {GRCORE_ACTIVATION_HOST, GRCORE_ACTIVATION_INTERPRETER,
          GRCORE_ACTIVATION_NATIVE, GRCORE_ACTIVATION_REENTRY}) {
    GRCORE_ActivationRef rec = enter(w.stack, kind);
    EXPECT_EQ(grcore_activation_set_compiled(w.stack, rec, 0x40, 0x50),
        GRCORE_ERR_INVALID) << kind;
    GRCORE_ActivationInfo info;
    ASSERT_EQ(grcore_activation_info(w.stack, rec, &info), GRCORE_OK);
    EXPECT_EQ(info.frame_base, 0u);
  }
  EXPECT_TRUE(walk_compiled(w.ctx).frames.empty());
}

TEST(ActivationCompiled, ARecordThatIsNotInnermostCanBeSetAndBadArgumentsAreRefused) {
  CWorld w;
  GRCORE_ActivationRef outer = enter(w.stack, GRCORE_ACTIVATION_JIT);
  GRCORE_ActivationRef inner = enter(w.stack, GRCORE_ACTIVATION_NATIVE);
  EXPECT_EQ(grcore_activation_set_compiled(w.stack, outer, 0x40, 0x50), GRCORE_OK);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, inner, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0u) << "the other record is untouched";
  EXPECT_EQ(grcore_activation_set_compiled(w.stack, outer, 0, 0x50), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_set_compiled(w.stack, GRCORE_ActivationRef{0}, 8, 8),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_set_compiled(w.stack, GRCORE_ActivationRef{999}, 8, 8),
      GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_activation_set_compiled(nullptr, outer, 8, 8), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_activation_info(w.stack, outer, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0x40u) << "a refusal changes nothing";
  EXPECT_EQ(info.return_address, 0x50u);
  std::thread([&] {
    EXPECT_EQ(grcore_activation_set_compiled(w.stack, outer, 8, 8), GRCORE_ERR_INVALID);
  }).join();
  // A left record is stale.
  grcore_activation_leave(w.stack, inner);
  EXPECT_EQ(grcore_activation_set_compiled(w.stack, inner, 8, 8), GRCORE_ERR_INVALID);
}

/* ---- The walk ----------------------------------------------------------- */

TEST(CompiledWalk, FiftyFramesAreYieldedInnermostFirstEachWithItsCodeSiteAndBase) {
  CWorld w;
  HandStack st(50);
  ChainRun run = set_up_run(w, st, 0, 50, 0);
  Walked got = walk_compiled(w.ctx);
  ASSERT_EQ(got.frames.size(), 50u);
  EXPECT_EQ(got.end, GRCORE_CWALK_END);
  for (size_t k = 0; k < 50; k++) {
    const GRCORE_CompiledFrame & f = got.frames[k];
    EXPECT_EQ(f.frame_base, run.bases[k]) << k;
    EXPECT_EQ(f.return_address, w.code.at(k)) << k;
    EXPECT_EQ(f.engine, w.alpha) << k;
    EXPECT_EQ(f.meta, &w.code.meta) << k;
    EXPECT_EQ(f.site, &w.code.sites[k]) << k;
    EXPECT_EQ(f.identity.function, 100 + k) << k;
    EXPECT_EQ(f.identity.offset, k) << k;
    EXPECT_EQ(f.depth, k);
    EXPECT_EQ(f.record, 0u);
    EXPECT_EQ(f.base_frames, 0u);
  }
}

TEST(CompiledWalk, NoStateMeansNothingToWalkWhateverElseTheStackHolds) {
  CWorld w;
  EXPECT_TRUE(walk_compiled(w.ctx).frames.empty()); // no record at all
  GRCORE_CSegment seg = {0x1000, 0x2000};
  GRCORE_ActivationRef host = enter(w.stack, GRCORE_ACTIVATION_HOST, &seg);
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  enter(w.stack, GRCORE_ACTIVATION_NATIVE, &seg);
  Walked got = walk_compiled(w.ctx);
  EXPECT_TRUE(got.frames.empty());
  EXPECT_EQ(got.end, GRCORE_CWALK_END);
  // State that was set and cleared is no state.
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, jit, 0x1000, w.code.at(0)), GRCORE_OK);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, jit, 0, 0), GRCORE_OK);
  EXPECT_TRUE(walk_compiled(w.ctx).frames.empty());
  (void)host;
  // A context with no engine walks as empty too.
  RunWorld bare;
  EXPECT_TRUE(walk_compiled(bare.ctx).frames.empty());
  GRCORE_CompiledWalk walk;
  EXPECT_EQ(grcore_compiled_walk_begin(nullptr, &walk), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_compiled_walk_begin(w.ctx, nullptr), GRCORE_ERR_INVALID);
}

TEST(CompiledWalk, ARunEndsAtTheEntryFromTheInterpreterAndTheNextRunIsFoundThroughItsRecord) {
  CWorld w;
  // Inner run: two frames at words 0..15. A native's gap. Outer run: three
  // frames at words 28..51.
  HandStack st(5, 12);
  GRCORE_ActivationRef outer = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> outer_bases = st.build(w.code, 28, 3, 2, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, outer, outer_bases[0], w.code.at(2)),
      GRCORE_OK);
  GRCORE_CSegment gap = {st.base_at(16) - 48, st.base_at(27) - 48};
  enter(w.stack, GRCORE_ACTIVATION_NATIVE, &gap);
  enter(w.stack, GRCORE_ACTIVATION_REENTRY);
  enter(w.stack, GRCORE_ACTIVATION_INTERPRETER, nullptr, w.alpha);
  GRCORE_ActivationRef inner = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> inner_bases = st.build(w.code, 0, 2, 0, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, inner, inner_bases[0], w.code.at(0)),
      GRCORE_OK);
  Walked got = walk_compiled(w.ctx);
  ASSERT_EQ(got.frames.size(), 5u);
  EXPECT_EQ(got.end, GRCORE_CWALK_END);
  EXPECT_EQ(got.frames[0].frame_base, inner_bases[0]);
  EXPECT_EQ(got.frames[1].frame_base, inner_bases[1]);
  EXPECT_EQ(got.frames[2].frame_base, outer_bases[0]);
  EXPECT_EQ(got.frames[3].frame_base, outer_bases[1]);
  EXPECT_EQ(got.frames[4].frame_base, outer_bases[2]);
  EXPECT_EQ(got.frames[0].record, 4u);
  EXPECT_EQ(got.frames[2].record, 0u);
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(got.frames[i].depth, i);
    EXPECT_EQ(got.frames[i].identity.offset, i) << "frames of both runs, in order";
  }
}

TEST(CompiledWalk, EveryBrokenChainIsAnErrorAndNeverASkipOrAnEnd) {
  struct Case {
    const char * name;
    size_t frames_before_the_break;
    // Mutates the built frames, the record's state and its segment.
    std::function<void(HandStack &, CWorld &, std::vector<uintptr_t> &,
        uintptr_t &, uintptr_t &, GRCORE_CSegment &)> mutate;
  };
  const Case cases[] = {
      {"the innermost return address is in no registered code", 0,
          [](HandStack &, CWorld &, std::vector<uintptr_t> &, uintptr_t &,
              uintptr_t & ret, GRCORE_CSegment &) { ret = kOutside; }},
      {"the innermost frame base is not word-aligned", 0,
          [](HandStack &, CWorld &, std::vector<uintptr_t> &, uintptr_t & base,
              uintptr_t &, GRCORE_CSegment &) { base += 4; }},
      {"a caller base equals its callee's", 3,
          [](HandStack & st, CWorld &, std::vector<uintptr_t> & b, uintptr_t &,
              uintptr_t &, GRCORE_CSegment &) { st.words[16 + 6] = b[2]; }},
      {"a caller base is below its callee's", 3,
          [](HandStack & st, CWorld &, std::vector<uintptr_t> & b, uintptr_t &,
              uintptr_t &, GRCORE_CSegment &) { st.words[16 + 6] = b[0]; }},
      {"a caller base is not word-aligned", 4,
          [](HandStack & st, CWorld &, std::vector<uintptr_t> & b, uintptr_t &,
              uintptr_t &, GRCORE_CSegment &) { st.words[24 + 6] = b[4] + 4; }},
      {"a caller base is beyond the record's segment", 4,
          [](HandStack & st, CWorld &, std::vector<uintptr_t> & b, uintptr_t &,
              uintptr_t &, GRCORE_CSegment & seg) {
            seg = {st.lo(), b[3] + 16};
          }},
      {"the innermost frame base is below the record's segment", 0,
          [](HandStack &, CWorld &, std::vector<uintptr_t> & b, uintptr_t &,
              uintptr_t &, GRCORE_CSegment & seg) { seg = {b[0] + 8, b[5] + 16}; }},
      {"a return word met mid-run is in no registered code and no marker was", 3,
          [](HandStack & st, CWorld &, std::vector<uintptr_t> &, uintptr_t &,
              uintptr_t &, GRCORE_CSegment &) { st.words[16 + 7] = kOutside; }},
      {"the last frame's marker is gone (zero) where the entry stub leaves it", 6,
          [](HandStack & st, CWorld &, std::vector<uintptr_t> &, uintptr_t &,
              uintptr_t &, GRCORE_CSegment &) { st.words[40 + 6] = 0; }},
      {"the last frame's marker is a word that merely looks like a base", 6,
          [](HandStack & st, CWorld &, std::vector<uintptr_t> & b, uintptr_t &,
              uintptr_t &, GRCORE_CSegment &) { st.words[40 + 6] = b[5] + 64; }},
      {"a return address is in code but at no site", 4,
          [](HandStack & st, CWorld & w, std::vector<uintptr_t> &, uintptr_t &,
              uintptr_t &, GRCORE_CSegment &) { st.words[24 + 7] = w.code.at(4) + 1; }},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    CWorld w;
    HandStack st(6);
    std::vector<uintptr_t> bases = st.build(w.code, 0, 6, 0, kOutside);
    uintptr_t base = bases[0], ret = w.code.at(0);
    GRCORE_CSegment seg = {0, 0};
    c.mutate(st, w, bases, base, ret, seg);
    GRCORE_ActivationRef rec =
        enter(w.stack, GRCORE_ACTIVATION_JIT, seg.hi > seg.lo ? &seg : nullptr);
    ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, base, ret), GRCORE_OK);
    Walked got = walk_compiled(w.ctx);
    EXPECT_EQ(got.end, GRCORE_CWALK_BROKEN);
    EXPECT_FALSE(got.reason.empty());
    EXPECT_EQ(got.frames.size(), c.frames_before_the_break)
        << "the frames before the break are all reported, and none after it";
  }
}

TEST(CompiledWalk, TheMarkerAloneEndsARunWhateverReturnAddressSitsNextToIt) {
  // The marker is what is definitive: the entry stub's return address is in
  // the interpreter's C code, in no registered code, and that is the entry.
  CWorld w;
  HandStack st(4);
  std::vector<uintptr_t> bases = st.build(w.code, 0, 4, 0, kOutside);
  EXPECT_EQ(st.words[3 * 8 + 6], GRCORE_COMPILED_CHAIN_END);
  EXPECT_EQ(GRCORE_COMPILED_CHAIN_END % 2, 1u);
  GRCORE_ActivationRef rec = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, bases[0], w.code.at(0)),
      GRCORE_OK);
  Walked got = walk_compiled(w.ctx);
  EXPECT_EQ(got.frames.size(), 4u);
  EXPECT_EQ(got.end, GRCORE_CWALK_END);
  // The same with a return address that is in registered code: the marker still
  // ends it, and nothing is read as a frame beyond it.
  st.words[3 * 8 + 7] = w.code.at(9);
  got = walk_compiled(w.ctx);
  EXPECT_EQ(got.frames.size(), 4u);
  EXPECT_EQ(got.end, GRCORE_CWALK_END);
}

TEST(CompiledWalk, EachFrameSaysWhereItIsInItsRunAndHowLongTheRunIs) {
  CWorld w;
  HandStack st(8 + 3);
  // An outer run of three under a nested activation, then an inner run of four.
  std::vector<uintptr_t> outer = st.build(w.code, 32, 3, 4, kOutside);
  std::vector<uintptr_t> inner = st.build(w.code, 0, 4, 0, kOutside);
  GRCORE_ActivationRef a = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, a, outer[0], w.code.at(4)),
      GRCORE_OK);
  GRCORE_ActivationRef n = enter(w.stack, GRCORE_ACTIVATION_NATIVE);
  GRCORE_ActivationRef b = enter(w.stack, GRCORE_ACTIVATION_JIT);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, b, inner[0], w.code.at(0)),
      GRCORE_OK);
  Walked got = walk_compiled(w.ctx);
  ASSERT_EQ(got.frames.size(), 7u);
  for (size_t i = 0; i < 4; i++) {
    EXPECT_EQ(got.frames[i].run_depth, i);
    EXPECT_EQ(got.frames[i].run_length, 4u);
  }
  for (size_t i = 4; i < 7; i++) {
    EXPECT_EQ(got.frames[i].run_depth, i - 4);
    EXPECT_EQ(got.frames[i].run_length, 3u);
  }
  ASSERT_EQ(grcore_activation_leave(w.stack, b), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, n), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, a), GRCORE_OK);
}

TEST(CompiledWalk, ARunBelowTheRunInsideItIsABrokenChain) {
  CWorld w;
  HandStack st(4);
  // The outer record's frames are at the LOW addresses, the inner run's high:
  // the chain claims the inner run is the outer one's callee, which it cannot be.
  GRCORE_ActivationRef outer = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> outer_bases = st.build(w.code, 0, 2, 2, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, outer, outer_bases[0], w.code.at(2)),
      GRCORE_OK);
  GRCORE_ActivationRef inner = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> inner_bases = st.build(w.code, 16, 2, 0, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, inner, inner_bases[0], w.code.at(0)),
      GRCORE_OK);
  Walked got = walk_compiled(w.ctx);
  EXPECT_EQ(got.end, GRCORE_CWALK_BROKEN);
  EXPECT_EQ(got.frames.size(), 2u) << "the inner run, then the break";
}

TEST(CompiledWalk, FramesReturningIntoRetiredCodeAreStillWalked) {
  CWorld w;
  HandStack st(6);
  ChainRun run = set_up_run(w, st, 0, 6, 0);
  ASSERT_EQ(grcore_code_unregister(w.ctx, w.code.start()), GRCORE_OK);
  GRCORE_CodeRange r;
  ASSERT_TRUE(grcore_code_lookup(w.ctx, w.code.at(0), &r));
  EXPECT_TRUE(r.retired);
  Walked got = walk_compiled(w.ctx);
  EXPECT_EQ(got.frames.size(), 6u) << "retired is not gone: a frame can return into it";
  EXPECT_EQ(got.end, GRCORE_CWALK_END);
  // Once the record leaves the code is released, and only then is it gone.
  ASSERT_EQ(grcore_activation_leave(w.stack, run.rec), GRCORE_OK);
  EXPECT_FALSE(grcore_code_lookup(w.ctx, w.code.at(0), &r));
}

/* ---- Roots -------------------------------------------------------------- */

TEST(CompiledRoots, FiftyFramesEveryStackMapReferenceIsVisitedOnceAndNothingElseIs) {
  CWorld w;
  HandStack st(50);
  set_up_run(w, st, 0, 50, 0);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(seen.values.size(), 50u);
  for (size_t k = 0; k < 50; k++) {
    EXPECT_EQ(seen.values[k], HandStack::ref_of(k)) << "frame " << k << ", innermost first";
    EXPECT_EQ(seen.slots[k], &st.words[k * 8 + 5]) << "the slot itself, not a copy";
  }
  // Each address once.
  EXPECT_EQ(std::set<uint64_t *>(seen.slots.begin(), seen.slots.end()).size(), 50u);
  // The raw words, the integer and the words outside any map are never reported,
  // and with no segment nothing is scanned conservatively.
  for (uint64_t value : seen.values) {
    EXPECT_LT(value, 0x900000u);
  }
  EXPECT_TRUE(seen.ranges.empty());
}

TEST(CompiledRoots, ARewriteBySomeoneWhoMovedTheObjectLandsInTheNativeFrame) {
  CWorld w;
  HandStack st(50);
  set_up_run(w, st, 0, 50, 0);
  Seen seen;
  seen.rewrite_add = 0x7000;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  for (size_t k = 0; k < 50; k++) {
    EXPECT_EQ(st.words[k * 8 + 5], HandStack::ref_of(k) + 0x7000) << k;
    // Nothing else in the frame moved: not the raw look-alike, not the I32,
    // not the words outside the map, not the saved base or return address.
    EXPECT_EQ(st.words[k * 8 + 4], HandStack::trap_of(k));
    EXPECT_EQ(st.words[k * 8 + 3], 7 + k);
    EXPECT_EQ(st.words[k * 8 + 2], HandStack::trap_of(k) + 1);
    EXPECT_EQ(st.words[k * 8 + 7], k + 1 < 50 ? w.code.at(k + 1) : kOutside);
  }
  // A second enumeration sees the new values: the slot really changed.
  Seen again;
  GRCORE_RootVisitor v2 = visitor_for(&again);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v2), GRCORE_OK);
  EXPECT_EQ(again.values[7], HandStack::ref_of(7) + 0x7000);
}

/* ---- Derived pointers (AD-12, CAP-10) ----------------------------------- */

namespace {
/* A visitor that moves what it is shown the way a moving collector does:
 * every reference becomes `value + shift`. It never sees the derived slot. */
struct Mover {
  uint64_t shift;
  std::vector<uint64_t *> seen;
};
GRCORE_RootVisitor mover_for(Mover * m) {
  GRCORE_RootVisitor v = {};
  v.user = m;
  v.slot = [](void * user, uint64_t * slot) {
    auto * mv = static_cast<Mover *>(user);
    mv->seen.push_back(slot);
    *slot += mv->shift;
  };
  return v;
}
} // namespace

TEST(CompiledRoots, EveryDerivedPointerOfASiteFollowsTheSharedBaseAndKeepsItsOwnHeldDelta) {
  CWorld w(GRCORE_UNLIMITED, false, false, /*derived=*/true);
  ASSERT_EQ(grcore_codemeta_validate(&w.code.meta, w.code.size(), nullptr), GRCORE_OK);
  HandStack st(50);
  set_up_run(w, st, 0, 50, 0);
  // The held deltas are what the code holds, and differ from the metadata's
  // 16 and 24 in some frames: the pass keeps the held ones, and each entry its own.
  auto delta0 = [](size_t k) { return k % 5 == 0 ? 40u : 16u; };
  auto delta1 = [](size_t k) { return k % 3 == 0 ? 72u : 24u; };
  for (size_t k = 0; k < 50; k++) {
    st.words[k * 8 + 2] = HandStack::ref_of(k) + delta0(k);
    st.words[k * 8 + 1] = HandStack::ref_of(k) + delta1(k);
  }
  Mover mover{0x70000, {}};
  GRCORE_RootVisitor v = mover_for(&mover);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(mover.seen.size(), 50u) << "the base is shown once, and no derived slot is";
  for (size_t k = 0; k < 50; k++) {
    uint64_t base = HandStack::ref_of(k) + 0x70000;
    EXPECT_EQ(st.words[k * 8 + 5], base) << k;
    EXPECT_EQ(st.words[k * 8 + 2], base + delta0(k)) << "first derived, frame " << k;
    EXPECT_EQ(st.words[k * 8 + 1], base + delta1(k)) << "second derived, frame " << k;
    // Nothing else moved.
    EXPECT_EQ(st.words[k * 8 + 4], HandStack::trap_of(k));
    EXPECT_EQ(st.words[k * 8 + 3], 7 + k);
  }
}

TEST(CompiledRoots, ADerivedPointerIsRewrittenToTheSameValueWhenNothingMoves) {
  CWorld w(GRCORE_UNLIMITED, false, false, /*derived=*/true);
  HandStack st(4);
  set_up_run(w, st, 0, 4, 0);
  for (size_t k = 0; k < 4; k++) {
    st.words[k * 8 + 2] = HandStack::ref_of(k) + 16;
    st.words[k * 8 + 1] = HandStack::ref_of(k) + 24;
  }
  Seen seen; // reads, writes nothing
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  for (size_t k = 0; k < 4; k++) {
    EXPECT_EQ(st.words[k * 8 + 5], HandStack::ref_of(k));
    EXPECT_EQ(st.words[k * 8 + 2], HandStack::ref_of(k) + 16);
    EXPECT_EQ(st.words[k * 8 + 1], HandStack::ref_of(k) + 24);
  }
}

TEST(CompiledRoots, AVisitorThatWantsOnlyRangesLeavesTheDerivedSlotAlone) {
  CWorld w(GRCORE_UNLIMITED, false, false, /*derived=*/true);
  HandStack st(2);
  set_up_run(w, st, 0, 2, 0);
  st.words[2] = 0x1234;
  Seen seen;
  seen.take_slots = false;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(st.words[2], 0x1234u);
}

TEST(CompiledRoots, AMixedChainScansOnlyTheNativeSegmentConservatively) {
  CWorld w;
  HandStack st(5, 12);
  GRCORE_ActivationRef outer = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> outer_bases = st.build(w.code, 28, 3, 2, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, outer, outer_bases[0], w.code.at(2)),
      GRCORE_OK);
  // The native's span is the words between the two runs; they hold look-alikes.
  for (size_t i = 16; i < 28; i++) {
    st.words[i] = HandStack::ref_of(i);
  }
  GRCORE_CSegment gap = {st.base_at(16) - 48, st.base_at(27) - 48};
  enter(w.stack, GRCORE_ACTIVATION_NATIVE, &gap, w.beta);
  enter(w.stack, GRCORE_ACTIVATION_REENTRY);
  GRCORE_ActivationRef inner = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> inner_bases = st.build(w.code, 0, 2, 0, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, inner, inner_bases[0], w.code.at(0)),
      GRCORE_OK);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  // Five compiled frames, precisely, inner run first.
  ASSERT_EQ(seen.values.size(), 5u);
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(seen.values[i], HandStack::ref_of(i)) << i;
  }
  // And exactly one range: the native's, with the engine its record names.
  ASSERT_EQ(seen.ranges.size(), 1u);
  EXPECT_EQ(seen.ranges[0].lo, gap.lo);
  EXPECT_EQ(seen.ranges[0].hi, gap.hi);
  EXPECT_EQ(seen.ranges[0].mask, 0xFFFF0u) << "beta's decoder";
}

/* The words of the stack that lie in a range, as a set of indices. */
std::set<size_t> words_in(HandStack & st, uint64_t lo, uint64_t hi) {
  std::set<size_t> out;
  for (size_t i = 0; i < st.words.size(); i++) {
    uintptr_t a = reinterpret_cast<uintptr_t>(&st.words[i]);
    if (a >= lo && a < hi) {
      out.insert(i);
    }
  }
  return out;
}

TEST(CompiledRoots, ASegmentThatContainsCompiledFramesIsScannedOnlyBetweenThem) {
  CWorld w;
  // Words 0..15 an inner run of two frames, 16..27 a native's gap, 28..51 an
  // outer run of three: the whole span is one record's segment.
  HandStack st(5, 12);
  GRCORE_CSegment whole = {st.lo(), st.hi()};
  GRCORE_ActivationRef outer = enter(w.stack, GRCORE_ACTIVATION_JIT, &whole);
  std::vector<uintptr_t> outer_bases = st.build(w.code, 28, 3, 2, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, outer, outer_bases[0], w.code.at(2)),
      GRCORE_OK);
  GRCORE_ActivationRef inner = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> inner_bases = st.build(w.code, 0, 2, 0, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, inner, inner_bases[0], w.code.at(0)),
      GRCORE_OK);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(seen.values.size(), 5u);
  std::set<size_t> scanned;
  for (const GRCORE_ConservativeRange & r : seen.ranges) {
    EXPECT_LT(r.lo, r.hi);
    for (size_t i : words_in(st, r.lo, r.hi)) {
      EXPECT_TRUE(scanned.insert(i).second) << "word " << i << " reported twice";
    }
  }
  // Exactly the words that are in no compiled frame: the gap.
  std::set<size_t> expected;
  for (size_t i = 16; i < 28; i++) {
    expected.insert(i);
  }
  EXPECT_EQ(scanned, expected);
  (void)outer;
  (void)inner;
}

TEST(CompiledRoots, ASegmentThatCutsAFrameInTwoKeepsOnlyWhatIsOutsideTheFrame) {
  CWorld w;
  HandStack st(5, 12);
  // The segment starts in the middle of the inner run's second frame (word 12)
  // and ends in the gap (word 20): only words 16..19 are outside a frame.
  GRCORE_CSegment cut = {reinterpret_cast<uintptr_t>(&st.words[12]),
      reinterpret_cast<uintptr_t>(&st.words[20])};
  GRCORE_ActivationRef rec = enter(w.stack, GRCORE_ACTIVATION_JIT);
  enter(w.stack, GRCORE_ACTIVATION_NATIVE, &cut);
  std::vector<uintptr_t> bases = st.build(w.code, 0, 2, 0, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, bases[0], w.code.at(0)),
      GRCORE_OK);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(seen.ranges.size(), 1u);
  EXPECT_EQ(words_in(st, seen.ranges[0].lo, seen.ranges[0].hi),
      (std::set<size_t>{16, 17, 18, 19}));
}

TEST(CompiledRoots, ACompiledFrameComesInPlaceAmongTheGuestFramesAndIsNotCountedTwice) {
  CWorld w;
  HandStack st(4);
  auto push = [&](uint64_t v) {
    GRCORE_FrameRef f;
    EXPECT_EQ(grcore_stack_push(w.stack, w.alpha, 2, &f), GRCORE_OK);
    grcore_stack_slot_set(w.stack, f, 0, v);
    grcore_stack_slot_set(w.stack, f, 1, v + 1);
  };
  push(10);                                      // G0
  GRCORE_ActivationRef a = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> ab = st.build(w.code, 16, 2, 2, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, a, ab[0], w.code.at(2)), GRCORE_OK);
  push(20);                                      // G1, above the first run
  GRCORE_ActivationRef b = enter(w.stack, GRCORE_ACTIVATION_JIT);
  std::vector<uintptr_t> bb = st.build(w.code, 0, 2, 0, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, b, bb[0], w.code.at(0)), GRCORE_OK);
  push(30);                                      // G2, innermost
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  // alpha's VALUE slot is the odd one: 31, then run b, 21, then run a, then 11.
  EXPECT_EQ(seen.values,
      (std::vector<uint64_t>{31, HandStack::ref_of(0), HandStack::ref_of(1), 21,
          HandStack::ref_of(2), HandStack::ref_of(3), 11}));
  grcore_unwind_all(w.stack, nullptr);
}

TEST(CompiledRoots, ARawEntryInAStackMapIsNeverReportedAsARoot) {
  // The validator lets a stack map hold a RAW frame slot; it is not a
  // reference, and handing it to a collector as a writable root is what AD-27
  // forbids. The raw word at -16 of each frame is the one that would be.
  CWorld w(GRCORE_UNLIMITED, /*raw_live=*/true);
  ASSERT_EQ(grcore_codemeta_validate(&w.code.meta, w.code.size(), nullptr), GRCORE_OK);
  HandStack st(3);
  set_up_run(w, st, 0, 3, 0);
  Seen seen;
  seen.rewrite_add = 1;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(seen.values.size(), 3u);
  for (size_t k = 0; k < 3; k++) {
    EXPECT_EQ(seen.values[k], HandStack::ref_of(k));
    EXPECT_EQ(st.words[k * 8 + 4], HandStack::trap_of(k)) << "the raw word was written";
  }
}

TEST(CompiledRoots, AGuestFrameThatCannotBeReadDoesNotHideTheCompiledFrames) {
  // The guest walk gives up at a frame whose header is damaged, as it always
  // has; the compiled frames are not guest frames and must still be reported,
  // not dropped with it.
  CWorld w;
  HandStack st(3);
  // The record is entered first, so its frames lie outside every guest frame.
  GRCORE_ActivationRef rec = enter(w.stack, GRCORE_ACTIVATION_JIT);
  GRCORE_FrameRef f1, f2;
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 2, &f1), GRCORE_OK);
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 2, &f2), GRCORE_OK);
  std::vector<uintptr_t> bases = st.build(w.code, 0, 3, 0, kOutside);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, bases[0], w.code.at(0)),
      GRCORE_OK);
  // Frame 2's tag is at byte 36 of its header: flip it.
  w.stack->buffer[f2.offset + 36] ^= 0xFF;
  ASSERT_FALSE(grcore_stack_frame_valid(w.stack, f2));
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(seen.values, (std::vector<uint64_t>{HandStack::ref_of(0),
                             HandStack::ref_of(1), HandStack::ref_of(2)}));
  w.stack->buffer[f2.offset + 36] ^= 0xFF;
  (void)f1;
  grcore_unwind_all(w.stack, nullptr);
}

TEST(CompiledRoots, RecordsWithSegmentsAndNoCompiledStateAreScannedWholeAsBefore) {
  CWorld w;
  GRCORE_CSegment seg = {0x4000, 0x4800};
  enter(w.stack, GRCORE_ACTIVATION_NATIVE, &seg);
  enter(w.stack, GRCORE_ACTIVATION_JIT, &seg);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  ASSERT_EQ(seen.ranges.size(), 2u);
  for (const auto & r : seen.ranges) {
    EXPECT_EQ(r.lo, 0x4000u);
    EXPECT_EQ(r.hi, 0x4800u);
  }
  EXPECT_TRUE(seen.values.empty());
}

TEST(CompiledRoots, ABrokenChainStopsTheEnumerationAndSaysWhyRatherThanSkippingAFrame) {
  struct Break {
    const char * name;
    std::function<void(HandStack &, CWorld &, uintptr_t &, uintptr_t &)> mutate;
    bool with_slots;
    bool with_ranges;
  };
  const Break cases[] = {
      {"no registered code", [](HandStack &, CWorld &, uintptr_t &, uintptr_t & ret) {
         ret = kOutside;
       }, true, true},
      {"caller at or below callee", [](HandStack & st, CWorld &, uintptr_t &, uintptr_t &) {
         st.words[8 + 6] = st.base_at(0);
       }, true, true},
      {"misaligned", [](HandStack & st, CWorld &, uintptr_t &, uintptr_t &) {
         st.words[8 + 6] += 4;
       }, true, true},
      {"no site", [](HandStack & st, CWorld & w, uintptr_t &, uintptr_t &) {
         st.words[8 + 7] = w.code.at(2) + 3;
       }, true, true},
      // A visitor that wants only ranges still has to walk to exclude frames.
      {"ranges only", [](HandStack & st, CWorld &, uintptr_t &, uintptr_t &) {
         st.words[8 + 6] = st.base_at(0);
       }, false, true},
  };
  for (const Break & b : cases) {
    SCOPED_TRACE(b.name);
#ifdef _WIN32
    GTEST_SKIP() << "root enumeration aborts, which is seen from a forked child";
#endif
    CWorld w;
    HandStack st(4);
    GRCORE_CSegment seg = {st.lo() - 4096, st.hi() + 4096};
    std::vector<uintptr_t> bases = st.build(w.code, 0, 4, 0, kOutside);
    uintptr_t base = bases[0], ret = w.code.at(0);
    b.mutate(st, w, base, ret);
    GRCORE_ActivationRef rec = enter(w.stack, GRCORE_ACTIVATION_JIT, &seg);
    ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, base, ret), GRCORE_OK);
    ChildResult r = in_child([&] {
      Seen seen;
      seen.take_slots = b.with_slots;
      seen.take_ranges = b.with_ranges;
      GRCORE_RootVisitor v = visitor_for(&seen);
      grcore_context_enumerate_roots(w.ctx, &v);
    });
    EXPECT_TRUE(r.aborted) << "it must not return with frames missing";
    EXPECT_NE(r.err.find("broken"), std::string::npos) << r.err;
    // The same chain refused in the parent by the walk, with a reason.
    EXPECT_EQ(walk_compiled(w.ctx).end, GRCORE_CWALK_BROKEN);
  }
}

TEST(CompiledRoots, AGoodChainDoesNotAbortInTheSameChild) {
  // The control for the case above: the same harness, nothing broken.
#ifdef _WIN32
  GTEST_SKIP() << "no fork on this platform";
#endif
  CWorld w;
  HandStack st(4);
  GRCORE_CSegment seg = {st.lo() - 4096, st.hi() + 4096};
  std::vector<uintptr_t> bases = st.build(w.code, 0, 4, 0, kOutside);
  GRCORE_ActivationRef rec = enter(w.stack, GRCORE_ACTIVATION_JIT, &seg);
  ASSERT_EQ(grcore_activation_set_compiled(w.stack, rec, bases[0], w.code.at(0)),
      GRCORE_OK);
  ChildResult r = in_child([&] {
    Seen seen;
    GRCORE_RootVisitor v = visitor_for(&seen);
    grcore_context_enumerate_roots(w.ctx, &v);
  });
  // (Not `exited_clean`: under Valgrind the child's leak report at exit sets a
  // status of its own, which says nothing about the chain.)
  EXPECT_FALSE(r.aborted);
  EXPECT_EQ(r.err.find("broken"), std::string::npos) << r.err;
}

/* ---- The abstract frame ------------------------------------------------- */

namespace {

/* Pushes guest frames and records, then pauses at a poll, so the frame walk
 * (which wants a paused or at-poll context) can read them. */
struct PauseGuest {
  std::function<void(GRCORE_Stack *)> setup;
  bool done = false;
  uint64_t poll_function = 3; // the poll names the innermost frame's identity
  uint64_t poll_offset = 7;
};
GRCORE_Step pause_entry(GRCORE_Context * c, void * state) {
  auto * g = static_cast<PauseGuest *>(state);
  if (!g->done) {
    g->done = true;
    g->setup(grcore_context_stack(c));
  }
  grcore_context_charge_fuel(c, 1);
  GRCORE_Verdict v = grcore_stack_poll(c, g->poll_function, g->poll_offset);
  return v == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED : GRCORE_STEP_FINISHED;
}

template <class W> void pause_with(W & w, PauseGuest & g) {
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, pause_entry, &g, &outcome), GRCORE_OK);
  ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
}

std::vector<GRCORE_AbstractFrame> walk_all(
    const GRCORE_Context * c, bool * broken = nullptr, std::string * why = nullptr) {
  std::vector<GRCORE_AbstractFrame> out;
  GRCORE_FrameWalk walk;
  EXPECT_EQ(grcore_frame_walk_begin(c, &walk), GRCORE_OK);
  GRCORE_AbstractFrame f;
  while (grcore_frame_walk_next(&walk, &f)) {
    out.push_back(f);
  }
  const char * reason = nullptr;
  bool b = grcore_frame_walk_broken(&walk, &reason);
  if (broken != nullptr) {
    *broken = b;
  }
  if (why != nullptr && reason != nullptr) {
    *why = reason;
  }
  return out;
}

/* The state of the interleaving test: three guest frames, and two compiled
 * runs of two frames each in between, as a native that re-entered guest code
 * leaves them. */
struct Interleaved {
  HandStack st{4};
  void setup(CWorld & w, GRCORE_Stack * s) {
    auto push = [&](uint64_t fn, uint64_t off) {
      GRCORE_FrameRef f;
      EXPECT_EQ(grcore_stack_push(s, w.alpha, 2, &f), GRCORE_OK);
      grcore_stack_slot_set(s, f, 0, fn * 10);
      grcore_stack_slot_set(s, f, 1, fn * 10 + 1);
      grcore_stack_set_identity(s, f, GRCORE_PollIdentity{fn, off});
    };
    push(1, 5);
    GRCORE_ActivationRef a = enter(s, GRCORE_ACTIVATION_JIT);
    std::vector<uintptr_t> ab = st.build(w.code, 16, 2, 2, kOutside);
    EXPECT_EQ(grcore_activation_set_compiled(s, a, ab[0], w.code.at(2)), GRCORE_OK);
    enter(s, GRCORE_ACTIVATION_NATIVE);
    push(2, 6);
    GRCORE_ActivationRef b = enter(s, GRCORE_ACTIVATION_JIT);
    std::vector<uintptr_t> bb = st.build(w.code, 0, 2, 0, kOutside);
    EXPECT_EQ(grcore_activation_set_compiled(s, b, bb[0], w.code.at(0)), GRCORE_OK);
    push(3, 7);
  }
};

} // namespace

TEST(CompiledFrame, CompiledFramesAppearInOrderBetweenTheGuestFramesWithTheirIdentity) {
  CWorld w(0);
  Interleaved scene;
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) { scene.setup(w, s); };
  pause_with(w, g);
  bool broken = true;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx, &broken);
  EXPECT_FALSE(broken);
  ASSERT_EQ(frames.size(), 7u);
  // Innermost first: G2, run b, G1, run a, G0.
  struct Want { bool compiled; uint64_t function, offset; };
  const Want want[] = {{false, 3, 7}, {true, 100, 0}, {true, 101, 1}, {false, 2, 6},
      {true, 102, 2}, {true, 103, 3}, {false, 1, 5}};
  for (size_t i = 0; i < 7; i++) {
    SCOPED_TRACE(i);
    EXPECT_EQ(frames[i].depth, i);
    EXPECT_EQ(frames[i].native_base != 0, want[i].compiled);
    EXPECT_EQ(frames[i].site != nullptr, want[i].compiled);
    EXPECT_EQ(frames[i].identity.function, want[i].function);
    EXPECT_EQ(frames[i].identity.offset, want[i].offset);
    EXPECT_EQ(frames[i].engine, w.alpha);
    EXPECT_EQ(frames[i].descriptor, &kAlpha);
    EXPECT_EQ(frames[i].context, w.ctx);
    EXPECT_STREQ(frames[i].location.file, kAlphaFile);
    EXPECT_EQ(frames[i].location.line, static_cast<int>(want[i].function * 1000 + want[i].offset));
    EXPECT_EQ(frames[i].slot_count, want[i].compiled ? 3u : 2u);
  }
  EXPECT_EQ(frames[1].native_base, scene.st.base_at(0));
  EXPECT_EQ(frames[2].native_base, scene.st.base_at(8));
  EXPECT_EQ(frames[4].native_base, scene.st.base_at(16));
  EXPECT_EQ(frames[1].site, &w.code.sites[0]);
  // The interpreter frames are still read as before.
  GRCORE_SlotKind kind;
  uint64_t value;
  ASSERT_EQ(grcore_frame_slot(&frames[3], 1, &kind, &value), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(value, 21u);
}

TEST(CompiledFrame, ACompiledFramesSlotsAreReadThroughItsMapAsTheyAreWithTheirRepresentation) {
  CWorld w(0);
  Interleaved scene;
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) { scene.setup(w, s); };
  pause_with(w, g);
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 7u);
  const GRCORE_AbstractFrame & f = frames[4]; // site 2
  GRCORE_SlotKind kind;
  uint64_t value;
  GRCORE_Representation rep;
  ASSERT_EQ(grcore_frame_slot_tagged(&f, 0, &kind, &value, &rep), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(value, HandStack::ref_of(2));
  EXPECT_EQ(rep, GRCORE_REPR_BITS);
  ASSERT_EQ(grcore_frame_slot_tagged(&f, 1, &kind, &value, &rep), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_RAW);
  EXPECT_EQ(value, HandStack::trap_of(2));
  EXPECT_EQ(rep, GRCORE_REPR_BITS);
  ASSERT_EQ(grcore_frame_slot_tagged(&f, 2, &kind, &value, &rep), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_RAW);
  EXPECT_EQ(value, 9u) << "the raw 32-bit word, not an engine value";
  EXPECT_EQ(rep, GRCORE_REPR_I32);
  // The plain accessor gives the same kind and bits; outputs may be omitted.
  ASSERT_EQ(grcore_frame_slot(&f, 2, &kind, &value), GRCORE_OK);
  EXPECT_EQ(value, 9u);
  EXPECT_EQ(grcore_frame_slot(&f, 2, nullptr, nullptr), GRCORE_OK);
  EXPECT_EQ(grcore_frame_slot_tagged(&f, 2, nullptr, nullptr, nullptr), GRCORE_OK);
  // Out of range, and an interpreter frame reports the representation BITS.
  EXPECT_EQ(grcore_frame_slot(&f, 3, &kind, &value), GRCORE_ERR_INVALID);
  ASSERT_EQ(grcore_frame_slot_tagged(&frames[0], 1, &kind, &value, &rep), GRCORE_OK);
  EXPECT_EQ(rep, GRCORE_REPR_BITS);
  // A compiled frame has no guest frame: the stack accessors refuse its ref,
  // and it has no scopes to ask the engine about.
  EXPECT_EQ(f.frame.offset, 0u);
  EXPECT_EQ(grcore_stack_slot_get(w.stack, f.frame, 0, &value), GRCORE_ERR_INVALID);
  EXPECT_EQ(grcore_frame_scope_count(&f), 0u);
  GRCORE_ScopeInfo scope;
  EXPECT_EQ(grcore_frame_scope(&f, 0, &scope), GRCORE_ERR_INVALID);
  // Inspecting goes through the engine's own inspector with the native word.
  char text[32];
  size_t length = 0;
  ASSERT_EQ(grcore_frame_inspect(&f, 2, text, sizeof text, &length), GRCORE_OK);
  EXPECT_STREQ(text, "a#9");
}

TEST(CompiledFrame, ReadingACompiledFrameNeverConvertsAnything) {
  // The engine converts, and counts: reading a frame by the walk must leave
  // both counts at zero (AD-27: no inspection triggers a conversion).
  ConvLog log;
  g_conv = &log;
  struct ConvWorld : CodeHolder, StackWorld {
    GRCORE_EngineId conv = 0;
    ConvWorld() : StackWorld(0) {
      EXPECT_EQ(grcore_engine_register(ctx, &kConv, &conv), GRCORE_OK);
      EXPECT_EQ(code.add_to(ctx, conv), GRCORE_OK);
    }
  } w;
  HandStack st(3);
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) {
    GRCORE_ActivationRef rec = enter(s, GRCORE_ACTIVATION_JIT);
    std::vector<uintptr_t> b = st.build(w.code, 0, 3, 0, kOutside);
    EXPECT_EQ(grcore_activation_set_compiled(s, rec, b[0], w.code.at(0)), GRCORE_OK);
  };
  pause_with(w, g);
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 3u);
  for (const GRCORE_AbstractFrame & f : frames) {
    EXPECT_EQ(f.engine, w.conv);
    for (size_t i = 0; i < f.slot_count; i++) {
      GRCORE_SlotKind kind;
      uint64_t value;
      GRCORE_Representation rep;
      ASSERT_EQ(grcore_frame_slot_tagged(&f, i, &kind, &value, &rep), GRCORE_OK);
    }
  }
  GRCORE_SlotKind kind;
  uint64_t value;
  GRCORE_Representation rep;
  ASSERT_EQ(grcore_frame_slot_tagged(&frames[1], 2, &kind, &value, &rep), GRCORE_OK);
  EXPECT_EQ(rep, GRCORE_REPR_I32);
  EXPECT_EQ(value, 8u) << "the raw word, not what conv_convert would make of it";
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(log.converts, 0);
  EXPECT_EQ(log.reverses, 0);
  g_conv = nullptr;
}

TEST(CompiledFrame, ADeadFrameStateSlotReadsAsRawZeroWithNoRepresentation) {
  CWorld w(0, /*raw_live=*/false, /*dead_slot=*/true);
  HandStack st(1);
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) {
    GRCORE_ActivationRef rec = enter(s, GRCORE_ACTIVATION_JIT);
    std::vector<uintptr_t> b = st.build(w.code, 0, 1, 0, kOutside);
    EXPECT_EQ(grcore_activation_set_compiled(s, rec, b[0], w.code.at(0)), GRCORE_OK);
  };
  GRCORE_FrameRef f;
  pause_with(w, g);
  (void)f;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  const GRCORE_AbstractFrame * c = nullptr;
  for (const auto & fr : frames) {
    if (fr.native_base != 0) {
      c = &fr;
    }
  }
  ASSERT_NE(c, nullptr);
  ASSERT_EQ(c->slot_count, 4u);
  GRCORE_SlotKind kind = GRCORE_SLOT_VALUE;
  uint64_t value = 77;
  GRCORE_Representation rep = GRCORE_REPR_I32;
  ASSERT_EQ(grcore_frame_slot_tagged(c, 3, &kind, &value, &rep), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_RAW) << "the site says VALUE, but nothing is there";
  EXPECT_EQ(value, 0u);
  EXPECT_EQ(rep, GRCORE_REPR_BITS);
}

TEST(CompiledFrame, ABreakFoundByTheLookaheadStillYieldsEveryGuestFrame) {
  // The record is entered first, so the compiled run lies outside the two
  // guest frames, and its innermost base is misaligned: the break is met when
  // the walk begins.
  CWorld w(0);
  HandStack st(2);
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) {
    GRCORE_ActivationRef rec = enter(s, GRCORE_ACTIVATION_JIT);
    std::vector<uintptr_t> b = st.build(w.code, 0, 2, 0, kOutside);
    EXPECT_EQ(grcore_activation_set_compiled(s, rec, b[0] + 4, w.code.at(0)), GRCORE_OK);
    GRCORE_FrameRef f;
    for (int i = 0; i < 2; i++) {
      EXPECT_EQ(grcore_stack_push(s, w.alpha, 1, &f), GRCORE_OK);
    }
  };
  pause_with(w, g);
  GRCORE_FrameWalk walk;
  ASSERT_EQ(grcore_frame_walk_begin(w.ctx, &walk), GRCORE_OK);
  GRCORE_AbstractFrame f;
  ASSERT_TRUE(grcore_frame_walk_next(&walk, &f));
  EXPECT_FALSE(grcore_frame_walk_broken(&walk, nullptr)) << "not yet: frames remain";
  ASSERT_TRUE(grcore_frame_walk_next(&walk, &f));
  EXPECT_FALSE(grcore_frame_walk_next(&walk, &f));
  const char * why = nullptr;
  EXPECT_TRUE(grcore_frame_walk_broken(&walk, &why));
  EXPECT_NE(why, nullptr);
}

TEST(CompiledFrame, GuestFramesBetweenAGoodCompiledFrameAndTheBreakAreStillYielded) {
  // G0, then a record whose run has one good frame and a bad caller, then G1:
  // the order is G1, the compiled frame, G0, and only then the break.
  CWorld w(0);
  HandStack st(2);
  PauseGuest g;
  g.poll_function = 2;
  g.poll_offset = 6;
  g.setup = [&](GRCORE_Stack * s) {
    GRCORE_FrameRef f;
    EXPECT_EQ(grcore_stack_push(s, w.alpha, 1, &f), GRCORE_OK);
    GRCORE_ActivationRef rec = enter(s, GRCORE_ACTIVATION_JIT);
    std::vector<uintptr_t> b = st.build(w.code, 0, 2, 0, kOutside);
    st.words[6] = b[0]; // the caller's base is its own
    EXPECT_EQ(grcore_activation_set_compiled(s, rec, b[0], w.code.at(0)), GRCORE_OK);
    EXPECT_EQ(grcore_stack_push(s, w.alpha, 1, &f), GRCORE_OK);
  };
  pause_with(w, g);
  bool broken = false;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx, &broken);
  EXPECT_TRUE(broken);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0].native_base, 0u);
  EXPECT_NE(frames[1].native_base, 0u);
  EXPECT_EQ(frames[2].native_base, 0u) << "the guest frame below the compiled one";
}

TEST(CompiledFrame, ABrokenChainEndsTheFrameWalkAfterWhatWasGoodAndSaysSo) {
  CWorld w(0);
  Interleaved scene;
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) {
    scene.setup(w, s);
    // Break the outer run: its first frame's caller base is its own.
    scene.st.words[16 + 6] = scene.st.base_at(16);
  };
  pause_with(w, g);
  bool broken = false;
  std::string why;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx, &broken, &why);
  EXPECT_TRUE(broken);
  EXPECT_FALSE(why.empty());
  // G2, run b (two), G1, the first frame of run a, and G0: the break is after
  // every good frame, and the guest frames below it are not dropped.
  ASSERT_EQ(frames.size(), 6u);
  EXPECT_EQ(frames[5].identity.function, 1u);
  EXPECT_EQ(frames[4].identity.function, 102u);
  // A walk that is not broken says it was not.
  CWorld ok(0);
  Interleaved fine;
  PauseGuest g2;
  g2.setup = [&](GRCORE_Stack * s) { fine.setup(ok, s); };
  pause_with(ok, g2);
  bool not_broken = true;
  EXPECT_EQ(walk_all(ok.ctx, &not_broken).size(), 7u);
  EXPECT_FALSE(not_broken);
  GRCORE_FrameWalk none = {};
  EXPECT_FALSE(grcore_frame_walk_broken(nullptr, nullptr));
  EXPECT_FALSE(grcore_frame_walk_broken(&none, nullptr));
}

TEST(CompiledFrame, AContextWithNoCompiledStateWalksExactlyAsBefore) {
  CWorld w(0);
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) {
    GRCORE_FrameRef f;
    for (uint64_t i = 1; i <= 3; i++) {
      EXPECT_EQ(grcore_stack_push(s, w.alpha, 1, &f), GRCORE_OK);
      grcore_stack_set_identity(s, f, GRCORE_PollIdentity{i, i});
    }
    enter(s, GRCORE_ACTIVATION_JIT); // a JIT record with no state
  };
  g.poll_function = 3;
  g.poll_offset = 3;
  pause_with(w, g);
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx);
  ASSERT_EQ(frames.size(), 3u);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(frames[i].native_base, 0u);
    EXPECT_EQ(frames[i].site, nullptr);
    EXPECT_EQ(frames[i].identity.function, 3 - i);
  }
}


/* ---- The walk-start cell (a/layout.h) ------------------------------------- */

namespace {

/* The cell, through the offset the layout descriptor states: the only way
 * compiled code knows it. */
uintptr_t * cell_of(GRCORE_Context * c) {
  return reinterpret_cast<uintptr_t *>(
      reinterpret_cast<unsigned char *>(c) + grcore_jit_layout()->walk_cell_offset);
}

} // namespace

TEST(WalkCell, TheLayoutNamesTwoWordsThatAreNeitherTheRequestWordNorEachOther) {
  const GRCORE_JitLayout * l = grcore_jit_layout();
  EXPECT_EQ(l->walk_cell_offset % sizeof(uintptr_t), 0u);
  EXPECT_EQ(l->native_limit_offset % sizeof(uintptr_t), 0u);
  EXPECT_GT(l->walk_cell_offset, 0u);
  EXPECT_NE(l->walk_cell_offset, l->request_word_offset);
  EXPECT_NE(l->native_limit_offset, l->request_word_offset);
  // The cell is two words, and the limit is not one of them.
  EXPECT_TRUE(l->native_limit_offset >= l->walk_cell_offset + 2 * sizeof(uintptr_t) ||
      l->native_limit_offset + sizeof(uintptr_t) <= l->walk_cell_offset);
}

TEST(WalkCell, AWalkStartsFromTheCellAndMovesItIntoTheInnermostRecord) {
  CWorld w;
  HandStack st(8);
  std::vector<uintptr_t> bases = st.build(w.code, 0, 3, 0, kOutside);
  GRCORE_ActivationRef rec = enter(w.stack, GRCORE_ACTIVATION_JIT);
  cell_of(w.ctx)[0] = bases[0];
  cell_of(w.ctx)[1] = w.code.at(0);
  Walked got = walk_compiled(w.ctx);
  ASSERT_EQ(got.frames.size(), 3u);
  EXPECT_EQ(got.frames[0].frame_base, bases[0]);
  EXPECT_EQ(got.frames[0].return_address, w.code.at(0));
  EXPECT_EQ(got.end, GRCORE_CWALK_END);
  // Moved, not copied: the cell is empty and the record carries it.
  EXPECT_EQ(cell_of(w.ctx)[0], 0u);
  EXPECT_EQ(cell_of(w.ctx)[1], 0u);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, rec, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, bases[0]);
  EXPECT_EQ(info.return_address, w.code.at(0));
  // The walk can be taken again, now from the record.
  EXPECT_EQ(walk_compiled(w.ctx).frames.size(), 3u);
  ASSERT_EQ(grcore_activation_leave(w.stack, rec), GRCORE_OK);
}

TEST(WalkCell, AnActivationEntryMovesTheCellIntoTheRecordBelowAndTheNewOneStartsEmpty) {
  CWorld w;
  HandStack st(8);
  std::vector<uintptr_t> bases = st.build(w.code, 0, 2, 0, kOutside);
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  cell_of(w.ctx)[0] = bases[0];
  cell_of(w.ctx)[1] = w.code.at(0);
  // A native the compiled code called records its own activation.
  GRCORE_ActivationRef native = enter(w.stack, GRCORE_ACTIVATION_NATIVE);
  EXPECT_EQ(cell_of(w.ctx)[0], 0u);
  GRCORE_ActivationInfo info;
  ASSERT_EQ(grcore_activation_info(w.stack, jit, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, bases[0]);
  EXPECT_EQ(info.return_address, w.code.at(0));
  ASSERT_EQ(grcore_activation_info(w.stack, native, &info), GRCORE_OK);
  EXPECT_EQ(info.frame_base, 0u);
  // A walk inside the native still sees the compiled run below it.
  EXPECT_EQ(walk_compiled(w.ctx).frames.size(), 2u);
  ASSERT_EQ(grcore_activation_leave(w.stack, native), GRCORE_OK);
  ASSERT_EQ(grcore_activation_leave(w.stack, jit), GRCORE_OK);
}

TEST(WalkCell, LeavingTheJitRecordClearsTheCellSoLaterWalksReadNoDeadFrames) {
  CWorld w;
  HandStack st(8);
  std::vector<uintptr_t> bases = st.build(w.code, 0, 2, 0, kOutside);
  GRCORE_ActivationRef jit = enter(w.stack, GRCORE_ACTIVATION_JIT);
  cell_of(w.ctx)[0] = bases[0];
  cell_of(w.ctx)[1] = w.code.at(0);
  ASSERT_EQ(grcore_activation_leave(w.stack, jit), GRCORE_OK);
  EXPECT_EQ(cell_of(w.ctx)[0], 0u);
  EXPECT_EQ(cell_of(w.ctx)[1], 0u);
  Walked got = walk_compiled(w.ctx);
  EXPECT_TRUE(got.frames.empty());
  EXPECT_EQ(got.end, GRCORE_CWALK_END);
}

TEST(WalkCell, ACellSetWhileTheInnermostRecordIsNotAJitOneIsABrokenWalkAndAborts) {
  CWorld w;
  HandStack st(8);
  std::vector<uintptr_t> bases = st.build(w.code, 0, 2, 0, kOutside);
  GRCORE_ActivationRef native = enter(w.stack, GRCORE_ACTIVATION_NATIVE);
  cell_of(w.ctx)[0] = bases[0];
  cell_of(w.ctx)[1] = w.code.at(0);
  Walked got = walk_compiled(w.ctx);
  EXPECT_TRUE(got.frames.empty());
  EXPECT_EQ(got.end, GRCORE_CWALK_BROKEN);
  EXPECT_NE(got.reason.find("no JIT activation"), std::string::npos) << got.reason;
#ifndef _WIN32
  // Root enumeration cannot return an error, so it stops the process, which is
  // seen from a forked child (there is no fork on Windows; the walk's verdict
  // above is the part of this test that runs there).
  ChildResult r = in_child([&] {
    Seen seen;
    GRCORE_RootVisitor v = visitor_for(&seen);
    grcore_context_enumerate_roots(w.ctx, &v);
  });
  EXPECT_TRUE(r.aborted) << r.err;
#endif
  cell_of(w.ctx)[0] = 0;
  cell_of(w.ctx)[1] = 0;
  ASSERT_EQ(grcore_activation_leave(w.stack, native), GRCORE_OK);
}

/* ---- A compiled frame and its guest frame are one frame (AD-28) ------------ */

namespace {

/* What a compiled call chain leaves: every guest call pushed the callee's guest
 * frame, so the interpreter's entry function and each callee below it have a
 * guest frame of their own, stale until a rebuild. Three compiled frames, the
 * innermost stopped at site 0 (function 100) and the outermost at site 2
 * (function 102), over guest frames 102, 101, 100 (outermost first), entered
 * with one guest frame on the stack. An optional extra frame sits above the
 * innermost: a callee that was pushed and has not been entered. */
struct PairedScene {
  HandStack st{3};
  void guest(CWorld & w, GRCORE_Stack * s, uint64_t fn, uint64_t first) {
    GRCORE_FrameRef f;
    EXPECT_EQ(grcore_stack_push(s, w.alpha, 2, &f), GRCORE_OK);
    grcore_stack_slot_set(s, f, 0, first);
    grcore_stack_slot_set(s, f, 1, first + 1); // alpha's VALUE slot
    grcore_stack_set_identity(s, f, GRCORE_PollIdentity{fn, 0});
  }
  void setup(CWorld & w, GRCORE_Stack * s, uint64_t extra_fn = 0,
      uint64_t innermost_fn = 100) {
    guest(w, s, 102, 10);
    GRCORE_ActivationRef rec = enter(s, GRCORE_ACTIVATION_JIT);
    std::vector<uintptr_t> b = st.build(w.code, 0, 3, 0, kOutside);
    EXPECT_EQ(grcore_activation_set_compiled(s, rec, b[0], w.code.at(0)), GRCORE_OK);
    guest(w, s, 101, 20);
    guest(w, s, innermost_fn, 30);
    if (extra_fn != 0) {
      guest(w, s, extra_fn, 40);
    }
  }
};

} // namespace

TEST(PairedFrames, EachCompiledFrameAndItsGuestFrameAreReportedAsRootsOnlyOnce) {
  CWorld w;
  PairedScene scene;
  scene.setup(w, w.stack);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  // The compiled frames' stack-map references, innermost first, and none of the
  // guest frames' stale VALUE slots (11, 21, 31).
  EXPECT_EQ(seen.values,
      (std::vector<uint64_t>{HandStack::ref_of(0), HandStack::ref_of(1),
          HandStack::ref_of(2)}));
  grcore_unwind_all(w.stack, nullptr);
}

TEST(PairedFrames, AGuestFrameAboveTheInnermostCompiledFrameIsStillAnOrdinaryFrame) {
  // A callee that was pushed and has not been entered has no compiled frame: it
  // is reported through its own slots, and the compiled frames below it keep
  // their pairing.
  CWorld w;
  PairedScene scene;
  scene.setup(w, w.stack, /*extra_fn=*/999);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(seen.values,
      (std::vector<uint64_t>{41, HandStack::ref_of(0), HandStack::ref_of(1),
          HandStack::ref_of(2)}));
  grcore_unwind_all(w.stack, nullptr);
}

TEST(PairedFrames, AGuestFrameThatIsNotTheCompiledFramesFunctionIsNotPairedAndNothingIsLost) {
  // The pairing is a claim about the stack; where it does not hold, the frames
  // are reported as before, which can retain too much and never too little.
  CWorld w;
  PairedScene scene;
  scene.setup(w, w.stack, 0, /*innermost_fn=*/555);
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  // The innermost compiled frame's guest frame is the wrong function, and the
  // one next to it is read as its own too, so both of those are reported; the
  // outermost pairs as it should and its stale slot (11) is not.
  std::multiset<uint64_t> got(seen.values.begin(), seen.values.end());
  for (uint64_t want : {HandStack::ref_of(0), HandStack::ref_of(1),
           HandStack::ref_of(2), uint64_t{21}, uint64_t{31}}) {
    EXPECT_EQ(got.count(want), 1u) << want;
  }
  EXPECT_EQ(got.size(), 5u);
  grcore_unwind_all(w.stack, nullptr);
}

TEST(PairedFrames, ARecordEnteredWithNoGuestFrameHasNothingToPairWith) {
  CWorld w;
  HandStack st(3);
  set_up_run(w, st, 0, 3, 0);
  GRCORE_FrameRef f;
  ASSERT_EQ(grcore_stack_push(w.stack, w.alpha, 2, &f), GRCORE_OK);
  grcore_stack_slot_set(w.stack, f, 1, 77);
  grcore_stack_set_identity(w.stack, f, GRCORE_PollIdentity{102, 0});
  Seen seen;
  GRCORE_RootVisitor v = visitor_for(&seen);
  ASSERT_EQ(grcore_context_enumerate_roots(w.ctx, &v), GRCORE_OK);
  EXPECT_EQ(std::multiset<uint64_t>(seen.values.begin(), seen.values.end()),
      (std::multiset<uint64_t>{HandStack::ref_of(0), HandStack::ref_of(1),
          HandStack::ref_of(2), 77}));
  grcore_unwind_all(w.stack, nullptr);
}

TEST(PairedFrames, TheAbstractWalkShowsEachPairedFrameOnceByTheCompiledFramesIdentity) {
  CWorld w(0);
  PairedScene scene;
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) { scene.setup(w, s, 0); };
  g.poll_function = 100;
  g.poll_offset = 0;
  pause_with(w, g);
  bool broken = true;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx, &broken);
  EXPECT_FALSE(broken);
  ASSERT_EQ(frames.size(), 3u);
  for (size_t i = 0; i < 3; i++) {
    SCOPED_TRACE(i);
    EXPECT_EQ(frames[i].depth, i);
    EXPECT_NE(frames[i].native_base, 0u);
    EXPECT_EQ(frames[i].identity.function, 100u + i);
    EXPECT_EQ(frames[i].frame.offset, 0u) << "the stack.h accessors stay refused";
    EXPECT_NE(frames[i].guest_frame.offset, 0u) << "the guest frame it stands for";
    // The slots are the compiled frame's, through its stack map.
    GRCORE_SlotKind kind;
    uint64_t value;
    ASSERT_EQ(grcore_frame_slot(&frames[i], 0, &kind, &value), GRCORE_OK);
    EXPECT_EQ(kind, GRCORE_SLOT_VALUE);
    EXPECT_EQ(value, HandStack::ref_of(i));
  }
  // The guest frames the compiled frames stand for are the three on the stack,
  // innermost first.
  GRCORE_FrameRef top = grcore_stack_top(w.stack);
  EXPECT_EQ(frames[0].guest_frame.offset, top.offset);
  EXPECT_EQ(frames[1].guest_frame.offset, grcore_stack_caller(w.stack, top).offset);
}

TEST(PairedFrames, AFrameWalkWithAnExtraGuestFrameShowsItFirstAndThenThePairedOnes) {
  CWorld w(0);
  PairedScene scene;
  PauseGuest g;
  g.setup = [&](GRCORE_Stack * s) { scene.setup(w, s, 999); };
  g.poll_function = 999;
  g.poll_offset = 0;
  pause_with(w, g);
  bool broken = true;
  std::vector<GRCORE_AbstractFrame> frames = walk_all(w.ctx, &broken);
  EXPECT_FALSE(broken);
  ASSERT_EQ(frames.size(), 4u);
  EXPECT_EQ(frames[0].native_base, 0u);
  EXPECT_EQ(frames[0].identity.function, 999u);
  EXPECT_EQ(frames[0].guest_frame.offset, 0u);
  for (size_t i = 1; i < 4; i++) {
    EXPECT_NE(frames[i].native_base, 0u);
    EXPECT_EQ(frames[i].identity.function, 99u + i);
    EXPECT_EQ(frames[i].depth, i);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
