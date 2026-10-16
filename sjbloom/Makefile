# Makefile for the sjbloom extension.
#
# It uses PGXS, PostgreSQL's build system for extensions. pg_config (found
# on PATH) tells PGXS where the installed PostgreSQL headers and folders are.
#
#   make              compile sjbloom.so
#   make install      copy sjbloom.so, sjbloom.control and the SQL file into
#                     the PostgreSQL install folder
#   make installcheck run the regression test in sql/ against the running
#                     server and compare with expected/

MODULE_big = sjbloom
OBJS = sjbloom.o bloom.o
EXTENSION = sjbloom
DATA = sjbloom--1.0.sql
REGRESS = sjbloom_basic
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
