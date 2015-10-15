#
# This makefile system follows the structuring conventions
# recommended sby Peter Miller in his excellent paper:
#
#	Recursive Make Considered Harmful
#	http://aegis.sourceforge.net/auug97.pdf
#
BINDIR := bin

# Lists all the makefrags add to
SRCDIRS :=
# Include Makefrags for subdirectories
include src/Makefrag

#TODO do this better
SIVFS_LIB_CFILES := \
	lib/sivfs_allocator.c

# How to make:
TOP = .

CC	:= gcc -pipe
CPP	:= g++ -pipe

PERL	:= perl

# Compiler flags
# -MD is needed for mergedep.pl to work don't remove!
CFLAGS := $(CFLAGS) -MD
CFLAGS += -I../kern -I../lib -Isrc

CFLAGS += -g -Wall -Wno-unused -Wpointer-arith -Wundef -fpermissive
CFLAGS += -O3

#C++ specific flags
CPPFLAGS := $(CFLAGS)
CPPFLAGS += -std=c++14

LDFLAGS :=

# Chacha
LDFLAGS += -lchacha

# Pthreads
CFLAGS += -pthread
LDFLAGS += -pthread

# Make sure that 'all' is the first target
all:

# Eliminate default suffix rules
.SUFFIXES:

# Delete target files if there is an error (or make is interrupted)
.DELETE_ON_ERROR:

# make it so that no intermediate .o files are ever deleted
#.PRECIOUS: %.o

# Set to nothing (i.e., V = ) to enable verbose outputs.
V = @

# Autoparallelize everything below
ifeq ($(MAKEFLAGS),)
%:
	@echo 'MAKEFLAGS not set, re-running make with -j8'
	@$(MAKE) -j8 $@
else

SIVFS_LIB_OBJFILES := $(patsubst %.c, $(BINDIR)/%.o, $(SIVFS_LIB_CFILES))
$(SIVFS_LIB_OBJFILES) : $(BINDIR)/%.o : ../%.c
	@echo + cpp $<
	@mkdir -p $(@D)
	$(V)$(CPP) $(CPPFLAGS) $(SITEVM_CFLAGS) -c -o $@ $<

# How to build C++ files
APPS_OBJFILES := $(patsubst %.cc, $(BINDIR)/%.o, $(APPS_CPPFILES))
SRC_OBJFILES := $(patsubst %.cc, $(BINDIR)/%.o, $(SRC_CPPFILES))

# compile (C++)
$(SRC_OBJFILES) $(APPS_OBJFILES) : $(BINDIR)/%.o : %.cc
	@echo + cpp $<
	@mkdir -p $(@D)
	$(V)$(CPP) $(CPPFLAGS) $(SITEVM_CFLAGS) -c -o $@ $<

TEST_OBJFILES := $(patsubst %.cc, $(BINDIR)/%.o, $(TEST_CPPFILES))
$(TEST_OBJFILES) : $(BINDIR)/src/unit_tests/%.o : src/unit_tests/%.cc src/unit_tests/test_framework.cc
	@echo + cpp $<
	@mkdir -p $(@D)
	$(V)$(CPP) $(CPPFLAGS) $(SITEVM_CFLAGS) \
         -I. "-DTEST_NAME=$*" "-DTEST_FILE=$<" \
         -c -o $@ src/unit_tests/test_framework.cc

#STAMP building
#Stamp code is in C but we compile it as C++
#Each stamp binary has a set of objects
STAMP_GENOME_OBJFILES := $(patsubst %.c, $(BINDIR)/%.o, $(STAMP_GENOME_CPPFILES))
STAMP_KMEANS_OBJFILES := $(patsubst %.c, $(BINDIR)/%.o, $(STAMP_KMEANS_CPPFILES))
STAMP_LABYRINTH_OBJFILES := $(patsubst %.c, $(BINDIR)/%.o, $(STAMP_LABYRINTH_CPPFILES))
STAMP_VACATION_OBJFILES := $(patsubst %.c, $(BINDIR)/%.o, $(STAMP_VACATION_CPPFILES))
STAMP_INTRUDER_OBJFILES := $(patsubst %.c, $(BINDIR)/%.o, $(STAMP_INTRUDER_CPPFILES))
STAMP_SSCA2_OBJFILES := $(patsubst %.c, $(BINDIR)/%.o, $(STAMP_SSCA2_CPPFILES))

STAMP_LIB_OBJFILES := $(patsubst %.c, $(BINDIR)/%.o, $(STAMP_LIB_CPPFILES))

#STAMP_OBJFILES := \
#	$(STAMP_GENOME_OBJFILES) \
#	$(STAMP_LIB_OBJFILES) \

STAMP_BINS := $(patsubst %.c, $(BINDIR)/%, $(STAMP_MAIN_CPPFILES))

#The frame-address warning is weird, possibly reenable if buggy stack dumps
#Some macros defined for specific benchmarks
STAMP_CFLAGS := -Isrc/stamp/lib -Isrc/stamp/compat -Wno-frame-address

# compile rule for stamp (C++)
$(BINDIR)/src/stamp/%.o : src/stamp/%.c
	@echo + cpp $<
	@mkdir -p $(@D)
	$(V)$(CPP) $(CPPFLAGS) $(STAMP_CFLAGS) $(SITEVM_CFLAGS) -c -o $@ $<

#Linker has the same args regardless of language of app
APPS_BINS := $(patsubst %.cc, $(BINDIR)/%, $(APPS_CPPFILES))
TEST_BINS := $(patsubst %.cc, $(BINDIR)/%, $(TEST_CPPFILES))
$(APPS_BINS) $(TEST_BINS) : $(BINDIR)/% : $(BINDIR)/%.o $(SRC_OBJFILES) $(SIVFS_LIB_OBJFILES)
	@mkdir -p $(@D)
	@echo + mk $@
	$(V)$(CPP) -o $@ $^ $(LDFLAGS)

$(BINDIR)/src/stamp/genome/genome : $(BINDIR)/% : $(STAMP_GENOME_OBJFILES) $(STAMP_LIB_OBJFILES) $(SRC_OBJFILES) $(SIVFS_LIB_OBJFILES)
	@mkdir -p $(@D)
	@echo + mk $@
	$(V)$(CPP) -o $@ $^ $(LDFLAGS)

$(BINDIR)/src/stamp/kmeans/kmeans : $(BINDIR)/% : $(STAMP_KMEANS_OBJFILES) $(STAMP_LIB_OBJFILES) $(SRC_OBJFILES) $(SIVFS_LIB_OBJFILES)
	@mkdir -p $(@D)
	@echo + mk $@
	$(V)$(CPP) -o $@ $^ $(LDFLAGS)

$(BINDIR)/src/stamp/labyrinth/labyrinth : $(BINDIR)/% : $(STAMP_LABYRINTH_OBJFILES) $(STAMP_LIB_OBJFILES) $(SRC_OBJFILES) $(SIVFS_LIB_OBJFILES)
	@mkdir -p $(@D)
	@echo + mk $@
	$(V)$(CPP) -o $@ $^ $(LDFLAGS)

$(BINDIR)/src/stamp/vacation/vacation : $(BINDIR)/% : $(STAMP_VACATION_OBJFILES) $(STAMP_LIB_OBJFILES) $(SRC_OBJFILES) $(SIVFS_LIB_OBJFILES)
	@mkdir -p $(@D)
	@echo + mk $@
	$(V)$(CPP) -o $@ $^ $(LDFLAGS)

$(BINDIR)/src/stamp/intruder/intruder : $(BINDIR)/% : $(STAMP_INTRUDER_OBJFILES) $(STAMP_LIB_OBJFILES) $(SRC_OBJFILES) $(SIVFS_LIB_OBJFILES)
	@mkdir -p $(@D)
	@echo + mk $@
	$(V)$(CPP) -o $@ $^ $(LDFLAGS)

$(BINDIR)/src/stamp/ssca2/ssca2 : $(BINDIR)/% : $(STAMP_SSCA2_OBJFILES) $(STAMP_LIB_OBJFILES) $(SRC_OBJFILES) $(SIVFS_LIB_OBJFILES)
	@mkdir -p $(@D)
	@echo + mk $@
	$(V)$(CPP) -o $@ $^ $(LDFLAGS)
	
#Matter of style - by marking all as phony and giving it a body
#(that does nothing) - make doesn't emit the "nothing to do" message
#which otherwise just clutters script output
all: $(APPS_BINS)
	@true

test: all $(TEST_BINS)
	@true

stamp: $(STAMP_BINS)
	@true

# For deleting the build
clean:
	rm -rf $(BINDIR)

clean-stamp:
	rm -rf $(BINDIR)/src/stamp

# This magic automatically generates makefile dependencies
# for header files included from C source files we compile,
# and keeps those dependencies up-to-date every time we recompile.
# See '../mergedep.pl' for more information.
#$(BINDIR)/.deps: $(foreach dir, $(SRCDIRS), $(wildcard $(BINDIR)/$(dir)/*.d))
DEPS_FILES := $(shell mkdir -p $(BINDIR) && find $(BINDIR)/ -type f -name '*.d')
$(BINDIR)/.deps: $(DEPS_FILES)
	@echo $^
	@mkdir -p $(@D)
	@$(PERL) ../build-dep/mergedep.pl $@ $^

-include $(BINDIR)/.deps

endif
