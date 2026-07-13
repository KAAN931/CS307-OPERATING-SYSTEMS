# CS307 — Operating Systems

Two systems-programming projects: a Unix shell, and an LC-3 virtual machine extended with a
working operating system (paging, processes, syscalls).

## `SUSHELL.c` — a Unix shell

A shell built directly on `fork`/`execvp`/`pipe`/`dup2`, supporting:

- **Pipelines** of arbitrary length — each command gets its own process, with pipe ends
  wired between them and carefully closed in the parent so readers actually see EOF.
- **I/O redirection** — `< infile` and `> outfile`, applied to the ends of the pipeline.
- **Loop-pipe** — a pipeline can be repeated *n* times with each iteration's stdout feeding
  the next iteration's stdin, so a filter can be applied repeatedly without rewriting it.

Execution is staged: an optional pipeline *before* the loop, the loop itself, and an
optional pipeline *after*, all chained together as one dataflow.

## `VirtualMemory&OSSimulator.c` — LC-3 VM + OS

An LC-3 instruction-set simulator (16-bit words, `R0`–`R7`, condition codes) with an
operating system layered on top of it:

- **Virtual memory** — 2048-word pages, per-process page tables located via a `PTBR`
  register, and a free-frame bitmap. Every memory access goes through translation, with
  read/write permission bits enforced per page (writing a read-only page is caught).
- **Processes** — PCBs holding `pid`, saved `PC`, and page-table base. Processes are
  created with a code segment and an initial heap, and can be loaded and switched.
- **Syscalls (traps)** — `tbrk` grows/shrinks the heap a page at a time, `tyld` yields the
  CPU to the next process, `thalt` terminates and frees the process's pages.

## Building

Both use the course-provided scaffolding headers — `parser.h` (the command-line parser,
supplying the `CmdVec` / `compiledCmd` types) for the shell, and `vm_dbg.h` for the VM.
With those alongside the sources:

```bash
gcc -o sushell SUSHELL.c
gcc -o vmsim "VirtualMemory&OSSimulator.c"
```

## Reports

The assignment specs and my write-ups are included as PDFs: `CS_307_PA1.pdf`,
`CS_307_PA4_FL25.pdf`, `report.pdf`, `reportassigment4.pdf`.

## License

[MIT](LICENSE)
