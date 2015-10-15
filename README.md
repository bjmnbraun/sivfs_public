# About this Repo

This is a git repository for /sysname , a Linux file-system providing snapshot
isolation transactions.

The codename for the system is /sysname = Thistle, so please refer to it
as such in documentation. On the other hand, all code should use code name "sivfs", short for
Snapshot Isolation VFS. Sivfs is a name used in code only because it is short,
but should be avoided in documentation since it is non-descriptive and possibly
non-unique.

See LICENSE for licensing information

Authors:
Benjamin A. Braun (bjmnbraun@gmail.com)

# Quick start

__*FIRST*__ do any thirdparty installation requirements mentioned in `test/README.md`

Then, it should be as easy as:

```
cd test
./make_mount_run.sh
```

which should produce some output ending with

```Main exited successfully!```

As well as some output to the kernel log, viewed by running:

```
dmesg
```

Where you should see
```
sivfs module inited
```

printed. Pay attention to any WARN or ERRORs printed out there - these are
serious and may require a hard reboot to resolve (!)

`make_mount_run.sh` builds and installs the kernel module (filesystem), mounts
an instance of it onto `/mnt/sivfs` (by default), and then runs a simple
userspace program that creates, mmaps, and performs a few transactions on the
file `/mnt/sivfs/test`.

The kernel module may fail to compile depending on your kernel
version. To troubleshoot this, see
[Compiling for a particular kernel version](#user-content-compiling-for-a-particular-kernel-version).

# How to use

### Build the kernel module

```
cd kern
make
```

### Install the kernel module

```
cd kern
make install
```

This inserts the built kernel module with `insmod` and also moves some headers
into `/usr/local/include/sivfs`

### Install the kernel module and mount the filesystem

```
cd kern
./mount_me.sh
```

This mounts the filesystem to /mnt/sivfs. It unmounts and unloads the sivfs
kernel module if already inserted.

### Build userspace applications

```
cd test
make
```

### Running userspace applications

```
cd test
./bin/src/apps/ycsb
```

### Cleaning up

```
cd kern
make clean #Just cleans the build directory for the kernel module
make uninstall #Uninstalls the kernel module and also removes files in /usr/local/include/sivfs
cd ..
cd test
make clean #Cleans up the build directory for the test programs
```

Also, running `./make_mount_run.sh` will unmount, unload, reload, and remount -
which can be useful if the kernel module got itself into a bad state.

# Code conventions: Kernel module

### Object creation / destruction

Methods that create or destroy objects come in one of five forms
* sivfs_init_* initializes an object in place
* sivfs_destroy_* destroys an object in place
* sivfs_new_* creates a new object and initializes it
* sivfs_get_* creates a new object and initializes it, unless it exists, in
        which case a reference count is incremented
* sivfs_put_* deletes an object returned from sivfs_new_* , or decrements the
        refcount (and deletes if necessary) an object returned from sivfs_get_*

TODO

# Code conventions: Userspace applications

### /sysname concurrency is inter-process, not inter-thread!

/sysname is designed to work with applications that spawn multiple child
processes, not multiple threads. See `test/src/apps/ycsb.cc` for how to spawn
multiple processes and have them communicate through a shared memory file. Each
process should then mmap any /sysname files it needs.

This simplifies discussion, because in /sysname all processes are
single-threaded processes, and hence a thread is synonymous with a process.

### Performing transactions on /sysname files

After mmap'ing one or more /sysname files (see `test/src/apps/ycsb.cc` for an example) a
thread can perform transactions by (1) performing any reads and writes as
usual, but keeping track of writes in a `sivfs_args_t` struct by calling
`sivfs_add_to_writeset` and then (2) calling the SIVFS_TXN_COMMIT_UPDATE IOCTL on `/dev/sivfs` passing
in the `sivfs_args_t` containing any writes, if any. The IOCTL will try to
commit the writes, returning commit/abort status, and in either case, will
upadte the thread's snapshot of memory to the most recent timestamp.

* The TM_WRITE and TM_READ and TM_COMMIT_UPDATE macros exported by
`tx_sys/sivfs.h` handle this for you.

### Allocation in transactions

TODO No current convention for something like a transactional malloc. The `intset_ll.cc`
application does dynamic allocation by preallocating pools of nodes to each
thread and using those pools, however this is not a general allocation scheme.

### Comparing to other STMs

The `test/tx_sys/` directory implements multiple STMs to aid in comparing
/sysname to other STMs. The following are defined:

* SIVFS : The snapshot-isolation protocol of /sysname
* GL : Global lock
* LOCKTABLE: Uses a table of locks, locks on read and writes.
* LOCKTABLE-SILO: Silo concurrency algorithm. Not a general-purpose STM since it does not
ensure reads are consistent within a transaction (conflicts are detected at
commit and aborts happen then.)
* LOCKTABLE-TINYSTM: TL2-style STM that uses timestamps to ensure read
consistency. Does not lock on reads.
* FGL : Fine-grained locking (must be implemented per-application)

Override the `TX_SYS` config variable to choose the STM to use,
see [Changing config parameters](#user-content-changing-config-parameters)
.

### Directory format

Applications are under `test/src`. To add an application, modify
`APPS_CPPFILES` in `test/src/Makefrag.`
To add a common file that should be linked into all applications, modify
`SRC_CPPFILES` in the same file.

### Metrics and application arguments

Applications should try to output useful metrics that can be immediately turned
into graphs. See examples such as `test/src/apps/ycsb.cc` where:

* Application arguments are printed in `dump_metrics`.
* Metrics specific to the application are added to `APP_METRICS`

TODO currently this is copy and pasted for each application, a template for an
application that does this automatically would be useful.

# Troubleshooting

### Weird behavior? Check `dmesg`

### Change verbosity of `dmesg` messages

Change the `DEBUG_` kernel module config parameters to enable / disable certain
debugging messages.
See [Changing config parameters](#user-content-changing-config-parameters)

### Changing config parameters

Both the kernel module and the test directory have config parameters in the
form of C preprocessor macros. After building once, there should be files:

```
kern/sivfs_config_local.h #For local config changes to kernel modules
test/txsys_config_local.h #For local config changes to test code
```

Each `_local` file contains local overrides for the same file without the
`_local` suffix. So, for example, to override the `SIVFS_LINUX_COMPAT` config
variable for the kernel module to support kernel versions newer than 4.6, I add

```
#undef SIVFS_COMPAT_LINUX
#define SIVFS_COMPAT_LINUX SIVFS_COMPAT_LINUX_4_6
```

to `kern/sivfs_config_local.h`.


__DO NOT MODIFY__ the base (i.e. not `_local` config files, these contain default settings
and are checked into the git repository. The `_local` files are mentioned in `.gitignore`
and should __never be checked in.__

### Compiling for a particular kernel version

First check your kernel version with 

```
uname -r
```

Usually the kernel version should start three numbers, i.e. ##.##.##, for
example 4.6.0. Then, open

```
cd kern
open sivfs_config.h
```

at the top of the file there should be some definitions like
`SIVFS_COMPAT_LINUX_XXX`. Each of these definitions defines a range of
supported kernel versions, and you should pick the latest definition that is at
least as old as your kernel version. For example, if I am on linux 4.7.5, I
choose `SIVFS_COMPAT_LINUX_4_6.`

Override `SIVFS_COMPAT_LINUX` with the setting you chose by changing a config
parameter, see [Changing config parameters](#user-content-changing-config-parameters)

# How it works: Code structure of kernel module

TODO system design section
