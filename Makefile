# Define the compiler and the flags
CLANG ?= clang
CFLAGS = -O2 -target bpf -c -g
USERSPACE_CFLAGS = -O2 -Wall -I/usr/include
USERSPACE_LINKER_FLAGS = -lbpf

# BPF programs
BPF_SRC = cpu_analyzer.bpf.c
BPF_OBJ = $(BPF_SRC:.c=.o)

# Userspace programs
USERSPACE_SRC = cpu_analyzer.c
USERSPACE_BIN = cpu_analyzer

all: $(BPF_OBJ) $(USERSPACE_BIN)

$(BPF_OBJ): $(BPF_SRC) vmlinux.h
	$(CLANG) $(CFLAGS) $(BPF_SRC) -o $(BPF_OBJ)

$(USERSPACE_BIN): $(USERSPACE_SRC)
	$(CLANG) -g $(USERSPACE_CFLAGS) $(USERSPACE_SRC) -o $(USERSPACE_BIN) $(USERSPACE_LINKER_FLAGS)

vmlinux.h:
		bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

clean:
	rm -f $(BPF_OBJ) $(USERSPACE_BIN)

cleanall: clean
	rm -f vmlinux.h

.PHONY: all clean
