#===============================================================================
# User Options
#===============================================================================

COMPILER  = clang
OPTIMIZE  = no
DEBUG     = no
PROFILE   = no

#===============================================================================
# Program name & source code list
#===============================================================================

program = rsbench

source = \
main.c \
simulation.c\
io.c \
init.c \
material.c \
utils.c \

obj = $(source:.c=.o)

#===============================================================================
# Sets Flags
#===============================================================================

# Standard Flags
CFLAGS := -std=gnu99 -Wall

# gcc Compiler
ifeq ($(COMPILER),gnu)
  CC = gcc
  CFLAGS += -fopenmp -ffast-math -flto
endif

# Intel Compiler
ifeq ($(COMPILER),intel)
  CC = icx 
  CFLAGS += -fiopenmp -fopenmp-targets=spir64 -D__STRICT_ANSI__ 
  # Optimization Flags
  ifeq ($(OPTIMIZE),yes)
    CFLAGS += -g
  endif
endif

# Clang-ykt compiler Targeting P100
ifeq ($(COMPILER),clang)
  CC = ~/dev/openmp-rpc-offloading/build/offloading/bin/clang
  # CFLAGS += -fopenmp -fopenmp-targets=x86_64-linux-gnu -lomptarget -L ~/dev/openmp-rpc-offloading/build/offloading/lib
  CFLAGS += -fopenmp -fopenmp-targets=nvptx64-cuda -Xopenmp-target -march=sm_75 -lomptarget -L ~/dev/openmp-rpc-offloading/build/offloading/lib
  ifeq ($(OPTIMIZE),yes)
    CFLAGS += -O3 -march=native
  endif
endif

# IBM XL Compiler Targeting K80
ifeq ($(COMPILER),ibm)
  CC = xlc_r
  CFLAGS += -qsmp=omp -qoffload
  ifeq ($(OPTIMIZE),yes)
    CFLAGS += -O2 # O3 gets the wrong verification answer
  endif
endif

# Linker Flags
LDFLAGS = -lm

# Debug Flags
ifeq ($(DEBUG),yes)
  CFLAGS += -g
  LDFLAGS += -g
endif

# Profiling Flags
ifeq ($(PROFILE),yes)
  CFLAGS += -pg
  LDFLAGS  += -pg
endif

#===============================================================================
# Targets to Build
#===============================================================================

$(program): $(obj) rsbench.h Makefile
	$(CC) $(CFLAGS) $(obj) -o $@ $(LDFLAGS)

%.o: %.c rsbench.h Makefile
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf rsbench $(obj)

edit:
	vim -p $(source) rsbench.h

run:
	./rsbench
