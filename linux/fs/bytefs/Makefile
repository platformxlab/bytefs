#
# Makefile for the linux BYTEFS filesystem routines.
#

obj-$(CONFIG_BYTEFS_FS) += bytefs.o

bytefs-y := balloc.o bbuild.o checksum.o dax.o dir.o file.o gc.o inode.o ioctl.o \
	journal.o log.o mprotect.o namei.o parity.o rebuild.o snapshot.o stats.o \
	super.o symlink.o sysfs.o perf.o ssd/bytefs_issue.o
