# Examples

Each program here is built and run by `make examples` (and by `make test`), so
an example that stops working fails the build. Run them with the same
`PREFIX=` the library was built with.

| To see how to... | Read |
| --- | --- |
| Run guest code under a fuel budget, see which key paused it and where, raise the budget and resume | [`pause_resume.c`](pause_resume.c), run 1 |
| Stop a paused run for good and read why it ended | [`pause_resume.c`](pause_resume.c), run 2 |
| Resume a paused context on a different thread | [`pause_resume.c`](pause_resume.c), run 3 |
| Write an entry function that can be paused and resumed without keeping a C frame | [`pause_resume.c`](pause_resume.c), `loop_entry` |

The examples use no engine: the "guest" is a counting loop whose position lives
in its own state. A real engine keeps that position on the guest stack.
