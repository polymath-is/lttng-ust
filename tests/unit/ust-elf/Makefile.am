AM_CPPFLAGS += -I$(top_srcdir)/tests/utils

noinst_PROGRAMS = ust-elf
ust_elf_SOURCES = ust-elf.c
ust_elf_LDADD = $(top_builddir)/liblttng-ust/liblttng-ust.la \
	$(top_builddir)/tests/utils/libtap.a

# Directories added to EXTRA_DIST will be recursively copied to the distribution.
EXTRA_DIST = \
	$(srcdir)/data \
	README.md
