# Makefile for the fixed_time kernel module
# Adapted from the Dune Kernel Module
# Adapted from the ipw2200 project.

# We have limited control over the build process of merge files - the kernel
# decides what compiler, how to do dependency tracking, etc.

ifndef CONFIG_SIVFS
EXTERNAL_BUILD=y
CONFIG_SIVFS=m
endif

#WEIRDness. The name of the module can't be equal to any of the
#constituent .c files, otherwise it only builds that one file.
list-m :=
list-$(CONFIG_SIVFS) += sivfs
obj-$(CONFIG_SIVFS) += sivfs.o
sivfs-objs := \
        sivfs_background_threads.o \
	sivfs_shared_state.o \
	sivfs_checkpoints.o \
        module.o \
        super.o \
        inode.o \
	sivfs_state.o \
        sivfs_commit_update_checkpoints.o \
        sivfs_inode_info.o \
        sivfs_make_checkpoint.o \
        sivfs_gc_checkpoints.o \
        sivfs_gc_log.o \
        sivfs_workingset.o \
        sivfs_stats.o \
        sivfs_mm.o \
        sivfs_nontxnal_ops.o \
	alloc/sivfs_allocation.o \

# KSRC should be set to the path to your sources
# modules are installed into KMISC
KVER  := $(shell uname -r)
KSRC := /lib/modules/$(KVER)/build
KMISC := /lib/modules/$(KVER)/kernel/drivers/misc
KMISC_INC := /lib/modules/$(KVER)/include

#SYSCALL_TBL = 0x$(shell $(EXTRACT) sys_call_table)
#DO_FORK = 0x$(shell $(EXTRACT) do_fork)

#Set to empty to print out whole commands for added targets
V := @

ccflags-y :=

ccflags-y += -Wno-unused
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wundef

ccflags-y += -O3
#ccflags-y += -O0

#Comment this to hide all douts
ccflags-y += -DDEBUG
#ggdb is a bit fancier than g, but not much
#ccflags-y += -ggdb
ccflags-y += -g
#?
#ccflags-y += -fno-omit-frame-pointer

#Static verification using smatch
ifdef SMATCH
ifdef RUN_SMATCH
CHECK := $(SMATCH) --project=kernel --full_path
CC := $(SMATCHCC)
CONFIG_DYNAMIC_DEBUG := n
endif
endif

# KSRC_OUTPUT should be overridden if you are using a 2.6 kernel that
# has it's output sent elsewhere via KBUILD_OUTPUT= or O=
KSRC_OUTPUT := $(KSRC)

PWD=$(shell pwd)

VERFILE := $(KSRC_OUTPUT)/include/linux/version.h
KERNELRELEASE := $(shell \
	if [ -r $(VERFILE) ]; then \
		(cat $(VERFILE); echo UTS_RELEASE) | \
		$(CC) -I$(KSRC_OUTPUT) $(CFLAGS) -E - | \
		tail -n 1 | \
		xargs echo; \
        else \
		uname -r; \
	fi)

MODPATH := $(DESTDIR)/lib/modules/$(KERNELRELEASE)

all: modules

lint: clean
	$(V) $(CPPLINT) \
        --extensions=c,h \
        *.c *.h \
	|& grep -v "Done processing" \
	|& grep -v "Total errors found" \
        || true

clean:
	rm -f *.mod.c *.mod *.o *.ko .*.cmd .*.flags .lst *.lst *.order *.symvers
	rm -f .*.d *.d
	rm -rf $(PWD)/tmp
	rm -rf $(PWD)/doc
#	for file in *.{c,h}; do \
#		sed -i -e "s:\ *$$::g" -e "s:\t*$$::g" $$file; \
	done

distclean: clean
	rm -f tags TAGS

TMP=$(PWD)/tmp
MODVERDIR=$(TMP)/.tmp_versions

#The actual compilation targets are run on a recursive make, so
#this setting of MAKEFLAGS will paralellize automatically.
#For some reason -j8 is not correct in MAKEFLAGS so -j 8 is needed
ifeq ($(MAKEFLAGS),)
MAKEFLAGS='-j 8'
endif

modules:
	mkdir -p $(MODVERDIR)
#	-cp $(IEEE80211_MODVERDIR)/*.mod $(MODVERDIR)
ifdef ($(KSRC_OUTPUT)/.tmp_versions)
	-cp $(KSRC_OUTPUT)/.tmp_versions/*.mod $(MODVERDIR)
endif
ifeq ($(KSRC),$(KSRC_OUTPUT)) # We're not outputting elsewhere
ifdef ($(KSRC)/.tmp_versions)
	-cp $(KSRC)/.tmp_versions/*.mod $(MODVERDIR)
endif
	$(MAKE) -C $(KSRC) M=$(PWD) MODVERDIR=$(MODVERDIR) modules \
	|& tee $(TMP)/make.out
	@if grep -i "WARN" $(TMP)/make.out > /dev/null; \
	then echo "There were warnings. Aborting."; \
        exit 1; \
	fi
	@if grep -i "ERROR" $(TMP)/make.out > /dev/null; \
	then echo "There were errors. Aborting."; \
        exit 1; \
	fi
else # We've got a kernel with seperate output, copy the config, and use O=
	mkdir -p $(TMP)
	cp $(KSRC_OUTPUT)/.config $(TMP)
	$(MAKE) -C $(KSRC) M=$(PWD) MODVERDIR=$(MODVERDIR)) O=$(PWD)/tmp modules
endif

install: modules
	sudo install -d $(KMISC)
	sudo install -m 644 -c $(addsuffix .ko,$(list-m)) $(KMISC)
	sudo mkdir -p /usr/local/include/sivfs
	sudo cp *.h /usr/local/include/sivfs
#        sudo /sbin/depmod -a ${KVER}
#	( sudo rmmod $(addsuffix .ko,$(list-m)) >& /dev/null ) || true
	sudo insmod $(addsuffix .ko,$(list-m))
	sleep 1
	sudo chmod ugo+rw /dev/sivfs

uninstall:
	sudo rm -rf $(addprefix $(KMISC),$(addsuffix .ko,$(list-m)))
	sudo /sbin/depmod -a ${KVER}
	sudo rm -rf /usr/local/include/sivfs

RCS_FIND_IGNORE := \( -name SCCS -o -name BitKeeper -o -name .svn -o -name CVS \) -prune -o
define all-sources
	( find . $(RCS_FIND_IGNORE) -name '*.[chS]' -print )
endef

.PHONY: TAGS tags check_inc

TAGS:
	$(all-sources) | etags -
tags:
	rm -f $@
	CTAGSF=`ctags --version | grep -i exuberant >/dev/null && echo "-I __initdata,__exitdata,EXPORT_SYMBOL,EXPORT_SYMBOL_NOVERS"`; \
	$(all-sources) | xargs ctags $$CTAGSF -a
