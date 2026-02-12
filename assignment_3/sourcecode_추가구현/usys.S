#include "syscall.h"
#include "traps.h"

#define SYSCALL(name) \
  .globl name; \
  name: \
    movl $SYS_ ## name, %eax; \
    int $T_SYSCALL; \
    ret

SYSCALL(fork)
SYSCALL(exit)
SYSCALL(wait)
SYSCALL(pipe)
SYSCALL(read)
SYSCALL(write)
SYSCALL(close)
SYSCALL(kill)
SYSCALL(exec)
SYSCALL(open)
SYSCALL(mknod)
SYSCALL(unlink)
SYSCALL(fstat)
SYSCALL(link)
SYSCALL(mkdir)
SYSCALL(chdir)
SYSCALL(dup)
SYSCALL(getpid)
SYSCALL(sbrk)
SYSCALL(sleep)
SYSCALL(uptime)
SYSCALL(dump_physmem_info)
SYSCALL(vtop)
SYSCALL(tlb_stats_sys)
SYSCALL(phys2virt)
SYSCALL(vm_alias_by_pa)
SYSCALL(vm_unmap_one)
SYSCALL(sw_tlb_reset)
SYSCALL(sw_tlb_read)
SYSCALL(vm_map_user_page)
SYSCALL(protect_nowrite)
