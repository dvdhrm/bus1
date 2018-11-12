bus1
====

Capability-based IPC for Linux

Bus1 is a Linux Subsystem to provide Object-oriented Inter-Process
Communication. It is a lightweight and scalable way for services and operating
system tasks to exchange messages, raise signals, and transfer data and
resources, while at the same time allowing modularization, privilege
separation, information hiding and isolation.

### Project

 - **Website**: <https://www.bus1.org>
 - **Bug-Tracker**: <https://github.com/bus1/bus1/issues>

### Requirements:

The requirements for this project are:

 * `linux-headers >= 5.0`

At build-time, additional requirements are:

 * `bc >= 1.0`
 * `GNU Make >= 4.2`

### Build

The kbuild Linux-Build-System is used to build this out-of-tree module. An
integration Makefile is provided in the top-level directory. To build the
kernel module, use:

```sh
make module
```

Note that only in-source builds are supported by kbuild when building
out-of-tree modules. Hence, a cleanup target is provided to remove all build
artifacts:

```sh
make clean
```

There is a test-suite that loads the built module and runs all available tests
against it. This should only be done on developer machines, since it loads the
module into your running kernel! This target will use `sudo` to load required
kernel modules and setup the test infrastructure. To run the test-suite, use:

```sh
make check
```

No install/uninstall targets are provided. Contact your distribution
documentation. For generic kernels, all you need is:

```sh
# (eg., MODLIB=/lib/modules/`uname -r`)
mkdir -p $(MODLIB)/kernel/ipc/bus1/
cp -f ipc/bus1/bus$(BUS1EXT).ko $(MODLIB)/kernel/ipc/bus1/
depmod $(KERNELVER)
```

### Repository:

 - **web**:   <https://github.com/bus1/bus1>
 - **https**: `https://github.com/bus1/bus1.git`
 - **ssh**:   `git@github.com:bus1/bus1.git`

### License:

 - **Apache-2.0** OR **GPL-2.0-or-later**
 - See AUTHORS file for details.
