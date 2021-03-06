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

/*
 * This tool is similar to readelf but for ecfs files. It parses and displays
 * output of ecfs files. (still in the works.
 */

#include <stdio.h>
#include "../ecfs_api/libecfs.h"

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("Usage: %s <ecfs_core>\n", argv[0]);
		exit(0);
	}
	int i, ret;
	ecfs_elf_t *desc;
	struct fdinfo *fdinfo;
	struct elf_prstatus *prstatus;
	ecfs_sym_t *dsyms, *lsyms;
	char *path;
	siginfo_t siginfo;
	Elf64_auxv_t *auxv;
	
	desc = load_ecfs_file(argv[1]);
	if (desc == NULL) {
		printf("Unable to load ecfs file\n");
		exit(-1);
	}

	path = get_exe_path(desc);
	if (path == NULL) {
		printf("Unable to retrieve executable path (is this an ecfs file?)\n");
		exit(-1);
	}

	printf("\e[1;31m- read_ecfs output for file\e[m %s\n", argv[1]);
	printf("\e[1;31m- Executable path (.exepath):\e[m %s\n", path);
	
	ret = get_thread_count(desc);
	printf("- \e[1;31mThread count (.prstatus):\e[m %d\n", ret);
	
	printf("- \e[1;31mThread info (.prstatus)\e[m\n");
	ret = get_prstatus_structs(desc, &prstatus);
	for (i = 0; i < ret; i++) 
		printf("\t\e[32m[thread\e[m %d] \e[32mpid:\e[m %d\n", i + 1, prstatus[i].pr_pid);
	printf("\n");

	ret = get_siginfo(desc, &siginfo);
        printf("\e[1;31m- Exited on signal (.siginfo):\e[m %d\n", siginfo.si_signo);
 	       
	
	ret = get_fd_info(desc, &fdinfo);
	printf("\e[1;31m- files/pipes/sockets (.fdinfo):\e[m\n");
	for (i = 0; i < ret; i++) {
		printf("\t\e[32m[fd:\e[m %d\e[32m] path:\e[m %s\n", fdinfo[i].fd, fdinfo[i].path);
		if (fdinfo[i].net) {
			switch(fdinfo[i].net) {
				case NET_TCP:
					printf("\t\e[1;32mPROTOCOL: TCP\e[m\n");
					printf("\t\e[1;32mSRC:\e[m %s:%d\n", inet_ntoa(fdinfo[i].socket.src_addr), fdinfo[i].socket.src_port);
					printf("\t\e[1;32mDST:\e[m %s:%d\n", inet_ntoa(fdinfo[i].socket.dst_addr), fdinfo[i].socket.dst_port);
					printf("\n");
					break;
				case NET_UDP:
					printf("\t\e[1;32mPROTOCOL: UDP\e[m\n");
                                        printf("\t\e[1;32mSRC:\e[m %s:%d\n", inet_ntoa(fdinfo[i].socket.src_addr), fdinfo[i].socket.src_port);
                                        printf("\t\e[1;32mDST:\e[m %s:%d\n", inet_ntoa(fdinfo[i].socket.dst_addr), fdinfo[i].socket.dst_port);
                                        printf("\n");
					break;
			}

		}
	}
	printf("\n");
	char **shlib_names;
	ret = get_shlib_mapping_names(desc, &shlib_names);
	printf("\e[1;31m- Printing shared library mappings:\e[m\n");
	for (i = 0; i < ret; i++) 
		printf("\e[32msharedlibs:\e[m\t%s\n", shlib_names[i]);
	printf("\n");

	ret = get_dynamic_symbols(desc, &dsyms);
	printf("\e[1;31m- Dynamic Symbol section -\e[m\n");
	for (i = 0; i < ret; i++)
		printf("\e[32m.dynsym:\e[m\t%s -\t %lx\n", &desc->dynstr[dsyms[i].nameoffset], dsyms[i].symval);
	printf("\n");
	ret = get_local_symbols(desc, &lsyms);
	printf("\e[1;31m- Symbol Table section -\e[m\n");
	for (i = 0; i < ret; i++)
		printf("\e[32m.symtab:\e[m\t %s -\t %lx\n", &desc->strtab[lsyms[i].nameoffset], lsyms[i].symval);
	printf("\n");

	pltgot_info_t *pltgot;
	if (!(desc->elfstats->personality & ELF_STATIC)) {
		printf("\e[1;31m- Printing out GOT/PLT characteristics (pltgot_info_t):\e[m\n");
		ret = get_pltgot_info(desc, &pltgot);
		for (i = 0; i < ret; i++) 
			printf("\e[32mgotsite\e[m: %lx \e[32mgotvalue:\e[m %lx \e[32mgotshlib:\e[m %lx \e[32mpltval:\e[m %lx\n", pltgot[i].got_site, pltgot[i].got_entry_va, 
			pltgot[i].shl_entry_va, pltgot[i].plt_entry_va);
		printf("\n");
	}

	int ac = get_auxiliary_vector64(desc, &auxv);
	printf("\e[1;31m- Printing auxiliary vector (.auxilliary):\e[m\n");
	for (i = 0; i < ac && auxv[i].a_type != AT_NULL; i++) {
		switch(auxv[i].a_type) {
			case AT_PHDR:
				printf("\e[32mAT_PHDR:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_PHENT:
				printf("\e[32mAT_PHENT:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_PHNUM:
				printf("\e[32mAT_PHNUM:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_PAGESZ:
				printf("\e[32mAT_PAGESZ:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_BASE:
				printf("\e[32mAT_BASE:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_EXECFD:
				printf("\e[32mAT_EXECFD:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_IGNORE:
				printf("\e[32mAT_IGNORE:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_ENTRY:
				printf("\e[32mAT_ENTRY:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_FLAGS:
				printf("\e[32mAT_FLAGS:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_UID:
				printf("\e[32mAT_UID:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_EUID:
				printf("\e[32mAT_EUID:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
			case AT_GID:
				printf("\e[32mAT_GID:\e[m\t 0x%lx\n", auxv[i].a_un.a_val);
				break;
		}
	}
	ElfW(Ehdr) *ehdr = desc->ehdr;
	ElfW(Shdr) *shdr = desc->shdr;
	char *shstrtab = desc->shstrtab;

	printf("\e[1;31m\n- Displaying ELF header:\e[m\n");
	
	printf("\e[32me_entry:\t\e[m 0x%lx\n"
		"\e[32me_phnum:\t\e[m %d\n"
		"\e[32me_shnum:\t\e[m %d\n"
		"\e[32me_shoff:\t\e[m 0x%lx\n"
		"\e[32me_phoff:\t\e[m 0x%lx\n" 
		"\e[32me_shstrndx:\t\e[m %d\n", ehdr->e_entry, ehdr->e_phnum, ehdr->e_shnum, 
				    ehdr->e_shoff, ehdr->e_phoff, ehdr->e_shstrndx);
	printf("\n\e[1;31m- Displaying ELF section headers:\e[m\n");
	printf("\e[32mAddress          Offset\t   Size\t   Entsize\t     Name\e[m\n");
	for (i = 0; i < desc->ehdr->e_shnum; i++) {
		printf("0x%-16lx 0x%-08lx 0x%-08lx 0x%-04lx %s\n", shdr[i].sh_addr, shdr[i].sh_offset, 
		shdr[i].sh_size, shdr[i].sh_entsize, &shstrtab[shdr[i].sh_name]);
	}			
	
}	

