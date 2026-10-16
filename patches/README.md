<!-- patches/: our patch files for contrib/postgres_fdw from the PostgreSQL 18 source (used from Batch 6 on). -->
# patches/

- `postgres_fdw-semijoin.patch` (Batch 6): the automatic semijoin rewrite in postgres_fdw. Made with
  `git -C ~/postgres/postgresql diff a85b5523e6..cs631-semijoin -- contrib/postgres_fdw`.
  Base: `a85b5523e6` (REL_18_STABLE); it also applies cleanly to plain `REL_18_6`.
  How to apply, build and undo it: docs/WALKTHROUGH.md, Batch 6.
  Since Batch 7 it also contains the regression test sql/semijoin.sql + expected/semijoin.out
  (needs sjbloom; run with `make installcheck REGRESS=semijoin`). Where everything lives:
  docs/WALKTHROUGH.md, "Where the code lives".
