# OpenMP Remote Offloading


## Build

Requirements:
- gRPC
- Protobuf
- clang/gcc to compile a clang that can compile to the desired target

If you have the latest CMake, I highly recommend using the "offloading" Preset, I've moved under the Project preset (./llvm/CMakePresets.json) so you can easily modify as needed. Otherwise, you can just use the preset as a baseline.

After you build, these are the relevant files that should've been compiled:
- ./bin/offloading-server
- ./lib/libomptarget.so || ./lib/libomptarget.so.12git
- ./lib/libomptarget.rtl.rpc.so
- Other device plugins depending on your system config/requirements

## Manual Configurations

Due to the nature of libomptarget and its device plugins, it often happens that the offloading server recognizes "too many devices" and then the offloaded application fails at runtime because the host libomptarget tries to execute a binary on an invalid device (i.e. CUDA binary on an X86). The solution at this point is to carefully pass library paths and remove unnecessary plugins from them so they don't accidentally get loaded. Pre-emptively, to make this easier, I've just commented out the plugin lines for x86 in openmp/libomptarget/src/rtl.cpp, so it won't get generated, and the remote offloading-server (presumably) won't see the remote host CPU as a device. You should be able to repeat a similar trick for ARM/etc. Additionally, be sure that you don't pass libomptarget.rtl.rpc.so to offloading-server otherwise it will also try to offload, and things break down.

The default address that the client/server listen are on is 0.0.0.0:50051. This is configurable via the environment var GRPC\_ADDRESS. If the application is to be offloaded onto multiple remotes, the var takes in multiple addresses as csvs. but the server application cannot take multiple addresses (it is not yet possible to have the same remote be offloaded onto from multiple hosts possibly simultanously from the same offloading-server, but it should be possible with multiple offloading-servers running at the same time on different addresses).

Additional env variables are:
- GRPC\_LATENCY (sets a timeout for some calls where a fixed timeout is reasonable such as initialization), default 5s, if you want to change the base from seconds, its at libomptarget/plugins/remote/src/Client.cpp:{67, 100}
- GRPC\_TRY (sets the number of tries before failing), default 2
- GRPC\_ALLOCATOR\_MAX (on the client side, we use an allocator for the messages, this sets the max allocated before it resets)
- GRPC\_BLOCK\_SIZE (sets the block size for large data transfer, it will send messages of at most this size)

## Testing
I've included two mini-apps under benchmarks with makefiles already mostly setup (other than the necessary system-specific configs) for remote offloading. You can find more documentation for them on their respective githubs for RSBench/XSBench. Both of these should work out of the box.

## Future

Most of the code written for the plugin wasn't analyzed beyond (does it work, does it have any obvious performance flaws), so the design/layout is less than ideal (so there may be spurious failures), but it is just a prototype. I'm dealing with these during my partial re-write where I try to use manual serialization and ucx instead of gRPC/protobuf. For HPC, I do believe that this is best case scenario but I'm open to suggestions.


# The LLVM Compiler Infrastructure

This directory and its sub-directories contain source code for LLVM,
a toolkit for the construction of highly optimized compilers,
optimizers, and run-time environments.

The README briefly describes how to get started with building LLVM.
For more information on how to contribute to the LLVM project, please
take a look at the
[Contributing to LLVM](https://llvm.org/docs/Contributing.html) guide.

## Getting Started with the LLVM System

Taken from https://llvm.org/docs/GettingStarted.html.

### Overview

Welcome to the LLVM project!

The LLVM project has multiple components. The core of the project is
itself called "LLVM". This contains all of the tools, libraries, and header
files needed to process intermediate representations and converts it into
object files.  Tools include an assembler, disassembler, bitcode analyzer, and
bitcode optimizer.  It also contains basic regression tests.

C-like languages use the [Clang](http://clang.llvm.org/) front end.  This
component compiles C, C++, Objective-C, and Objective-C++ code into LLVM bitcode
-- and from there into object files, using LLVM.

Other components include:
the [libc++ C++ standard library](https://libcxx.llvm.org),
the [LLD linker](https://lld.llvm.org), and more.

### Getting the Source Code and Building LLVM

The LLVM Getting Started documentation may be out of date.  The [Clang
Getting Started](http://clang.llvm.org/get_started.html) page might have more
accurate information.

This is an example work-flow and configuration to get and build the LLVM source:

1. Checkout LLVM (including related sub-projects like Clang):

     * ``git clone https://github.com/llvm/llvm-project.git``

     * Or, on windows, ``git clone --config core.autocrlf=false
    https://github.com/llvm/llvm-project.git``

2. Configure and build LLVM and Clang:

     * ``cd llvm-project``

     * ``mkdir build``

     * ``cd build``

     * ``cmake -G <generator> [options] ../llvm``

        Some common build system generators are:

        * ``Ninja`` --- for generating [Ninja](https://ninja-build.org)
          build files. Most llvm developers use Ninja.
        * ``Unix Makefiles`` --- for generating make-compatible parallel makefiles.
        * ``Visual Studio`` --- for generating Visual Studio projects and
          solutions.
        * ``Xcode`` --- for generating Xcode projects.

        Some Common options:

        * ``-DLLVM_ENABLE_PROJECTS='...'`` --- semicolon-separated list of the LLVM
          sub-projects you'd like to additionally build. Can include any of: clang,
          clang-tools-extra, libcxx, libcxxabi, libunwind, lldb, compiler-rt, lld,
          polly, or debuginfo-tests.

          For example, to build LLVM, Clang, libcxx, and libcxxabi, use
          ``-DLLVM_ENABLE_PROJECTS="clang;libcxx;libcxxabi"``.

        * ``-DCMAKE_INSTALL_PREFIX=directory`` --- Specify for *directory* the full
          path name of where you want the LLVM tools and libraries to be installed
          (default ``/usr/local``).

        * ``-DCMAKE_BUILD_TYPE=type`` --- Valid options for *type* are Debug,
          Release, RelWithDebInfo, and MinSizeRel. Default is Debug.

        * ``-DLLVM_ENABLE_ASSERTIONS=On`` --- Compile with assertion checks enabled
          (default is Yes for Debug builds, No for all other build types).

      * ``cmake --build . [-- [options] <target>]`` or your build system specified above
        directly.

        * The default target (i.e. ``ninja`` or ``make``) will build all of LLVM.

        * The ``check-all`` target (i.e. ``ninja check-all``) will run the
          regression tests to ensure everything is in working order.

        * CMake will generate targets for each tool and library, and most
          LLVM sub-projects generate their own ``check-<project>`` target.

        * Running a serial build will be **slow**.  To improve speed, try running a
          parallel build.  That's done by default in Ninja; for ``make``, use the option
          ``-j NNN``, where ``NNN`` is the number of parallel jobs, e.g. the number of
          CPUs you have.

      * For more information see [CMake](https://llvm.org/docs/CMake.html)

Consult the
[Getting Started with LLVM](https://llvm.org/docs/GettingStarted.html#getting-started-with-llvm)
page for detailed information on configuring and compiling LLVM. You can visit
[Directory Layout](https://llvm.org/docs/GettingStarted.html#directory-layout)
to learn about the layout of the source code tree.
