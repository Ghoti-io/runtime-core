# Examples

Each program here is built and run by `make examples` (and by `make test`), so
an example that stops working fails the build. Run them with the same
`PREFIX=` the library was built with.

| To see how to... | Read |
| --- | --- |
| Run guest code under a fuel budget, see which key paused it and where, raise the budget and resume | [`pause_resume.c`](pause_resume.c), run 1 |
| Stop a paused run for good and read why it ended | [`pause_resume.c`](pause_resume.c), run 2 |
| Resume a paused context on a different thread | [`pause_resume.c`](pause_resume.c), run 3 |
| Read a paused guest's frames, slots and scope variables without knowing the engine | [`frame_walk.c`](frame_walk.c), `main` |
| Write an engine descriptor: slot kinds, a locator, an inspector and a scope interface | [`frame_walk.c`](frame_walk.c), `toy_engine` |
| Keep a guest's position on the guest stack instead of in C frames, so a pause can be read | [`frame_walk.c`](frame_walk.c), `toy_entry` |
| Stop a runaway call at its own boundary while the page around it finishes (a budget scope per template call) | [`scoped_pages.c`](scoped_pages.c), `render_pane` |
| Unwind the guest stack to a scope's boundary, and see the engine's `unwind` hook release each frame | [`scoped_pages.c`](scoped_pages.c), `render_pane` and `toy_unwind` |
| Read how much of its own budget a scope used, and how the parent's clock stops while a child runs | [`scoped_pages.c`](scoped_pages.c), `main` |
| Freeze a paused context into a snapshot, restore it into two fresh contexts (one on another thread) and finish all three the same way | [`snapshot_resume.c`](snapshot_resume.c), `main` |
| Give a key snapshot hooks: write its state, rebuild it in a CHECK then an APPLY pass, undo it on ABANDON | [`snapshot_resume.c`](snapshot_resume.c), `loop_snapshot`, `loop_restore`, `loop_settle` |
| Write an entry function that can be paused and resumed without keeping a C frame | [`pause_resume.c`](pause_resume.c), `loop_entry` |

The examples use no engine. In `pause_resume.c` the "guest" is a counting loop
whose position lives in its own state; in `frame_walk.c` and `scoped_pages.c`
the frames are on the guest stack, as a real engine keeps them, and a toy
descriptor stands in for the engine.
