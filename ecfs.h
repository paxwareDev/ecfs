/*
 * Copyright (c) 2015, Ryan O'Neill
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <elf.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <errno.h>
#include <link.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <sys/user.h>
#include <sys/procfs.h>		/* struct elf_prstatus */
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include "dwarf.h"
#include "libdwarf.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_RAMDISK_GIGS 4
#define ECFS_RAMDISK_DIR "/mnt/ecfs_ramdisk"

/*
 * Custom sections
 */
#define SHT_INJECTED 0x200000 


/*
 * used in cases where we want to prevent an shdr for being written
 */
#define INVALID_SH_OFFSET (long)~0x0 
/*
 * Type of socket
 */
#define NET_TCP 1
#define NET_UDP 2

#define HUGE_ALLOC(size)  \
      mmap(0, (size), PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)

#define MAX_LIB_NAME 255
#define MAX_LIB_PATH 512

#define ECFS_CORE_DIR "/tmp" // XXX change this?
#define UNKNOWN_SHDR_SIZE 64
#define PAGE_ALIGN(x) (x & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x) (PAGE_ALIGN(x) + PAGE_SIZE)

#define MAX_TID 256
#define PT_ATTACHED 1
#define PT_DETACHED 2

#define PARTIAL_SNAPSHOT 1
#define COMPLETE_SNAPSHOT 0

#define MAXFD 255

#define PS_TRACED 1
#define PS_STOPPED 2
#define PS_SLEEP_UNINTER 4
#define PS_SLEEP_INTER 8
#define PS_DEFUNCT 16
#define PS_RUNNING 32
#define PS_UNKNOWN 64

#define MAX_THREADS 256		// for each threads prstatus

#define ELFNOTE_NAME(_n_) ((char*)(_n_) + sizeof(*(_n_)))
#define ELFNOTE_ALIGN(_n_) (((_n_)+3)&~3)
#define ELFNOTE_NAME(_n_) ((char*)(_n_) + sizeof(*(_n_)))
#define ELFNOTE_DESC(_n_) (ELFNOTE_NAME(_n_) + ELFNOTE_ALIGN((_n_)->n_namesz))
#define ELFNOTE_NEXT(_n_) ((ElfW(Nhdr) *)(ELFNOTE_DESC(_n_) + ELFNOTE_ALIGN((_n_)->n_descsz)))

#define MAX_SHDR_COUNT 2048

typedef struct elf_stats {
#define ELF_STATIC (1 << 1) // if its statically linked (instead of dynamically)
#define ELF_PIE (1 << 2)    // if its position indepdendent executable
#define ELF_LOCSYM (1 << 3) // local symtab exists?
#define ELF_HEURISTICS (1 << 4) // were detection heuristics used by ecfs?
#define ELF_STRIPPED_SHDRS (1 << 8)
        unsigned int personality; // if (personality & ELF_STATIC)
} elf_stat_t;

struct opts {
	int text_all; // write complete text segment (not just 4096 bytes) of each shared library
	int heuristics; // heuristics for detecting dll injection etc.
	int use_stdin;
	int use_ramdisk;
	char *logfile;
};

typedef struct {
	Elf64_Word namesz;
	Elf64_Word descsz;
	Elf64_Word type;
} ElfW(Note);

struct fde_func_data {		/* For eh_frame.c */
	uint64_t addr;
	size_t size;
};

#ifndef MAX_PATH
#define MAX_PATH 512
#endif

typedef struct fdinfo {
	int fd;
	char path[MAX_PATH];
	struct {
		struct in_addr src_addr;
		struct in_addr dst_addr;
		uint16_t src_port;
		uint16_t dst_port;
	} socket;
	char net;
} fd_info_t;

struct section_meta {
	ElfW(Addr) bssVaddr, dynVaddr, relVaddr, relaVaddr, ehframeVaddr,
	    textVaddr, o_textVaddr, dataVaddr, o_dataVaddr, gotVaddr, noteVaddr,
	    hashVaddr, initVaddr, finiVaddr, pltVaddr, dsymVaddr, dstrVaddr,
	    interpVaddr, tlsVaddr, plt_relaVaddr, plt_relVaddr;

	ElfW(Off) bssOff, dynOff, relOff, relaOff, noteOff, ehframeOff,
	    textOffset, dataOffset, gotOff, hashOff, initOff, finiOff, pltOff,
	    dsymOff, dstrOff, interpOff, tlsOff, plt_relaOff, plt_relOff;
	
	ElfW(Word) bssSiz, dynSiz, hashSiz, ehframeSiz, textfSize, textSize,
	    dataSize, strSiz, pltSiz, interpSiz, tlsSiz, noteSiz, dsymSiz,
	    dstrSiz, plt_relaSiz, plt_relSiz;
};

struct elf_thread_core_info {
	struct elf_prstatus *prstatus;
	struct elf_prpsinfo *psinfo;
	struct user_regs_struct *regs;
	elf_fpregset_t *fpu;
	siginfo_t *siginfo;
	 ElfW(Nhdr) * notes;
};

typedef struct ecfs_file_fmt {
	loff_t prstatus_offset; 
	loff_t prpsinfo_offset;
	loff_t fdinfo_offset;
	loff_t siginfo_offset;
	loff_t auxv_offset;
	loff_t exepath_offset;
	loff_t stb_offset;
	loff_t personality_offset;
	loff_t arglist_offset;
	size_t prstatus_size;
	size_t prpsinfo_size;
	size_t fdinfo_size;
	size_t siginfo_size;
	size_t auxv_size;
	size_t exepath_size;
	size_t personality_size;
	size_t arglist_size;
	int thread_count;
} ecfs_file_t;

struct nt_file_struct {
	struct {
		unsigned long addr;
		size_t size;
		size_t pgoff;
		char path[512];
	} files[4096];
	int fcount;
	int page_size;
};

struct lib_mappings {
	struct {
		unsigned long addr;
		unsigned long offset;
		size_t size;
		uint32_t flags; // PF_W|PF_R etc.
		int injected; // to signify that the file was an injected dll
		char name[MAX_LIB_NAME + 1];
		char path[MAX_LIB_PATH + 1];
	} libs[4096];
	int libcount;
};

typedef struct notedesc {
	ElfW(Nhdr) * notes;
	struct elf_prstatus *prstatus;	/* NT_PRSTATUS */
	struct elf_prpsinfo *psinfo;	/* NT_PRPSINFO */
#if __x86_64__
	Elf64_auxv_t *auxv;
#else
	Elf32_auxv_t *auxv;
#endif
	struct siginfo_t *siginfo;
	struct elf_thread_core_info thread_core_info[MAX_THREADS];
	elf_fpregset_t *fpu;
	int thread_count;
	int thread_status_size;
	int numnote;
	size_t auxv_size;
	struct nt_file_struct *nt_files;
	struct lib_mappings *lm_files;
} notedesc_t;

struct coredump_params {
	siginfo_t *siginfo;
	struct pt_regs *regs;
	unsigned long limit;
	unsigned long mm_flags;
};

typedef struct elfdesc {
	uint8_t *mem;
	 ElfW(Ehdr) * ehdr;
	 ElfW(Phdr) * phdr;
	 ElfW(Shdr) * shdr;
	 ElfW(Nhdr) * nhdr;
	 ElfW(Dyn)  *dyn;
	 ElfW(Addr) textVaddr;
	 ElfW(Addr) dataVaddr;
	 ElfW(Addr) dynVaddr;
	 ElfW(Addr) ehframe_Vaddr;
	 ElfW(Addr) noteVaddr;
	 ElfW(Addr) bssVaddr;
	 ElfW(Addr) interpVaddr;
	 ElfW(Off) textOffset;
	 ElfW(Off) dataOffset;
	 ElfW(Off) ehframeOffset;
	 ElfW(Off) dynOffset;
	 ElfW(Off) bssOffset;
	 ElfW(Off) interpOffset;
	 ElfW(Off) noteOffset;
	char *StringTable;
	char *path;
	size_t size;
	size_t noteSize;
	size_t gnu_noteSize;
	size_t textSize;
	size_t dataSize;
	size_t dynSize;
	size_t ehframe_Size;
	size_t bssSize;
	size_t interpSize;
	size_t o_datafsize; // data filesz of executable (not core)
	size_t text_memsz;
	size_t text_filesz;
	int dynlinked;
	int pie;
} elfdesc_t;

typedef struct mappings {
	uint8_t *mem;
	uint8_t *text_image; // allocated mapping containing text segment (only if shlib)
	char *filename;
	unsigned long base;
	size_t size;
	int elfmap;
	int textbase;		// is the text segment base of the processes exe
	int stack;
	int thread_stack;
	int heap;
	int shlib;	// this is a shared library
	int injected; // illegally injected
	int padding;  // this is padding or relro
	int special;
	int anonmap_exe;
	int filemap;
	int filemap_exe;
	int vdso;
	int vsyscall;
	int stack_tid;
	size_t sh_offset;
	uint32_t p_flags;
	int has_pt_load;
	ssize_t text_len; // only if an shlib is this used
} mappings_t;

typedef struct memdesc {
	pid_t pid;
	uint8_t *exe;		/* Points to /proc/<pid>/exe */
	char *path;		// path to executable (might be a symlnk)
	char *comm;		// filename of executable (might be a symlink)
	char *exe_path;		// path to executable (real path not symlink)
	char *exe_comm; 	// real filename, not symlink
	int mapcount;		// overall # of memory maps
	int type;		// ET_EXEC or ET_DYN
	uint8_t *textseg;	// gets heapallocated and has text segment read into it
	ElfW(Addr) base, data_base;
	
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} stack;
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} vdso;
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} vsyscall;
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} heap;
	struct {
		unsigned long sh_offset;
		unsigned long base;
		unsigned int size;
	} text; // special case since coredumps don't dump complete text image
	struct {
		int fds[MAXFD];
		int pid;
		int ppid;
		int uid;
		int gid;
		int tidcount;
		int exit_signal;
		pid_t tid[MAX_TID];
		pid_t leader;
		pid_t tracer;	// the pid of the tracer
		unsigned int state;
	} task;
	mappings_t *maps;
	struct user_regs_struct pt_regs;
	char *stack_args;
	size_t stack_args_len;
	uint8_t *saved_auxv;
	int pie;
	unsigned long o_entry; 
	fd_info_t *fdinfo;
	ssize_t fdinfo_size;
} memdesc_t;

typedef struct descriptor {
	elfdesc_t binary;
	memdesc_t memory;
	notedesc_t info[MAX_THREADS];
	int exe_type;
	int dynlinking;
	char *snapdir;
} desc_t;

typedef struct node {
	struct node *next;
	struct node *prev;
	void *data;
} node_t;

typedef struct list {
	node_t *head;
	node_t *tail;
} list_t;

typedef struct symentry {
        ElfW(Addr) value;
        size_t size;
        size_t count; //# of sym entries
	char *name;  //symname
	char *library; //libname
	
} symentry_t;

struct dlopen_libs {
	char *libname;
	char *libpath;
	int count;
};

struct needed_libs {
	char *libname; // just the name of the library
	char *libpath; // path to a library 
	char *master; // the library or executable that depends on libpath/libname
	int count;
};

/*
 * XXX smoothly transition these globals in somewhere else
 * these were added for another after the fact strangeness
 * to account for the fact that /proc/$pid disappears as soon
 * as the corefile has been read from stdin. so certain problems
 * don't show up when passing corefiles directly to ecfs that
 * do show up when use core_pattern. 
 */

struct {
        ssize_t hash_size;
        ssize_t rela_size;
        ssize_t init_size;
        ssize_t fini_size;
        ssize_t got_size;
        ssize_t ehframe_size;
        ssize_t plt_rela_size;
	ssize_t plt_size;
	unsigned long plt_vaddr;
        int eh_frame_offset_workaround;
	int stripped; // means section headers are stripped
} global_hacks;

void *heapAlloc(size_t);
char *xstrdup(const char *);
char *xfmtstrdup(char *fmt, ...);
int get_all_functions(const char *filepath, struct fde_func_data **funcs);
void ecfs_print(char *, ...);
int xopen(const char *, int);

/* from list.c */
int insert_item_end(list_t **list, void *data, size_t sz);
int insert_item_front(list_t **list, void *data, size_t sz);

/* from symresolve.c */

int fill_dynamic_symtab(list_t **list, struct lib_mappings *lm);
unsigned long lookup_from_symlist(const char *name, list_t *list);

/*
 * from ecfs.c
 */
ElfW(Off) get_internal_sh_offset(elfdesc_t *elfdesc, memdesc_t *memdesc, int type);
