#include "exec.h"
#include "mm.h"
#include "process.h"
#include "vfs.h"
#include <binfmt/elf.h>
#include <common/auxvec.h>
#include <common/errno.h>
#include <log.h>
#include <heap.h>

#include <Windows.h>

__declspec(noreturn) static void goto_entrypoint(const char *stack, void *entrypoint)
{
	__asm
	{
		mov eax, entrypoint
		mov esp, stack
		push eax
		xor eax, eax
		xor ebx, ebx
		xor ecx, ecx
		xor edx, edx
		xor esi, esi
		xor edi, edi
		xor ebp, ebp
		mov gs, ax
		ret
	}
}

static void run(Elf32_Ehdr *eh, void *pht, int argc, char *argv[], int env_size, char *envp[], PCONTEXT context)
{
	/* Generate initial stack */
	int aux_size = 7;
	int initial_stack_size = argc + 1 + env_size + 1 + aux_size * 2 + 1;
	char *stack_base = process_get_stack_base();
	const char **stack = (const char **)(stack_base + STACK_SIZE - initial_stack_size * sizeof(const char *) - sizeof(argc));
	int idx = 0;
	/* argc */
	stack[idx++] = argc;
	/* argv */
	for (int i = 0; i < argc; i++)
		stack[idx++] = argv[i];
	stack[idx++] = NULL;
	/* environment variables */
	for (int i = 0; i < env_size; i++)
		stack[idx++] = envp[i];
	stack[idx++] = NULL;
	/* auxiliary vector */
	stack[idx++] = (const char *)AT_PHDR;
	stack[idx++] = (const char *)pht;
	stack[idx++] = (const char *)AT_PHENT;
	stack[idx++] = (const char *)eh->e_phentsize;
	stack[idx++] = (const char *)AT_PHNUM;
	stack[idx++] = (const char *)eh->e_phnum;
	stack[idx++] = (const char *)AT_PAGESZ;
	stack[idx++] = (const char *)PAGE_SIZE;
	stack[idx++] = (const char *)AT_BASE;
	stack[idx++] = (const char *)NULL;
	stack[idx++] = (const char *)AT_FLAGS;
	stack[idx++] = (const char *)0;
	stack[idx++] = (const char *)AT_ENTRY;
	stack[idx++] = (const char *)eh->e_entry;
	stack[idx++] = NULL;

	/* Call executable entrypoint */
	uint32_t entrypoint = eh->e_entry;
	log_debug("Entrypoint: %x\n", entrypoint);
	/* If we're starting from main(), just jump to entrypoint */
	if (!context)
		goto_entrypoint(stack, entrypoint);
	/* Otherwise, we're at execve() in syscall handler context */
	/* TODO: Add a trampoline to free original stack */
	context->Eax = 0;
	context->Ecx = 0;
	context->Edx = 0;
	context->Ebx = 0;
	context->Esp = stack;
	context->Ebp = 0;
	context->Esi = 0;
	context->Edi = 0;
}

int do_execve(const char *filename, int argc, char *argv[], int env_size, char *envp[], PCONTEXT context)
{
	HANDLE hFile = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	/* Load ELF header */
	Elf32_Ehdr eh;
	ReadFile(hFile, &eh, sizeof(Elf32_Ehdr), NULL, NULL);
	if (eh.e_type != ET_EXEC)
	{
		log_debug("Not an executable!\n");
		return -1;
	}

	if (eh.e_machine != EM_386)
	{
		log_debug("Not an i386 executable.\n");
		return -1;
	}

	/* Load program header table */
	uint32_t phsize = (uint32_t)eh.e_phentsize * (uint32_t)eh.e_phnum;
	void *pht = kmalloc(phsize); /* TODO: Free it at execve */
	SetFilePointer(hFile, eh.e_phoff, NULL, FILE_BEGIN);
	ReadFile(hFile, pht, phsize, NULL, NULL);

	for (int i = 0; i < eh.e_phnum; i++)
	{
		Elf32_Phdr *ph = (Elf32_Phdr *)((uint8_t *)pht + (eh.e_phentsize * i));
		if (ph->p_type == PT_LOAD)
		{
			uint32_t addr = ph->p_vaddr;
			uint32_t size = ph->p_memsz + (addr & 0x00000FFF);
			addr &= 0xFFFFF000;

			int prot = 0;
			if (ph->p_flags & PF_R)
				prot |= PROT_READ;
			if (ph->p_flags & PF_W)
				prot |= PROT_WRITE;
			if (ph->p_flags & PF_X)
				prot |= PROT_EXEC;
			void *mem = sys_mmap((void *)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_ANONYMOUS, -1, 0);
			mm_update_brk((uint32_t)addr + size);
			SetFilePointer(hFile, ph->p_offset, NULL, FILE_BEGIN);
			ReadFile(hFile, (void *)ph->p_vaddr, ph->p_filesz, NULL, NULL);
		}
	}
	CloseHandle(hFile);
	run(&eh, pht, argc, argv, env_size, envp, context);
	return 0;
}

int sys_execve(const char *filename, char *const argv[], char *const envp[], int _4, int _5, PCONTEXT context)
{
	log_debug("execve(%s)\n", filename);
	log_debug("Reinitializing...");
	vfs_reset();
	mm_reset();
	return -1;
}