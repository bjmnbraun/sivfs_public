#
# This makefile system follows the structuring conventions
# recommended sby Peter Miller in his excellent paper:
#
#	Recursive Make Considered Harmful
#	http://aegis.sourceforge.net/auug97.pdf
#
OUTDIR := out

#File to read in to make graphs
PERF_DATA := ../perf.data

# Lists all the makefrags add to
SRCDIRS :=
# Include Makefrags for subdirectories

# How to make:
TOP = .

# Eliminate default suffix rules
.SUFFIXES:

# Delete target files if there is an error (or make is interrupted)
.DELETE_ON_ERROR:

.DEFAULT_GOAL := all

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

$(OUTDIR)/perf.data.flattened: $(PERF_DATA)
	$(V)mkdir -p $(@D)
	$(V)sudo perf script -i $(PERF_DATA) | ../thirdparty/stackcollapse-perf.pl > $@

$(OUTDIR)/flamegraph.svg: $(OUTDIR)/perf.data.flattened
	$(V)cat $^ | ../thirdparty/flamegraph.pl > $@
	$(V)echo "Flamegraph is in $@"

flamegraph: $(OUTDIR)/flamegraph.svg

#Matter of style - by marking all as phony and giving it a body
#(that does nothing) - make doesn't emit the "nothing to do" message
#which otherwise just clutters script output
all: flamegraph
	@true

# For deleting the build
clean:
	rm -rf $(OUTDIR)

endif
