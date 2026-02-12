#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"
#include "tlb.h"
#include "ipt.h"

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // ELF 헤더 확인
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf)) goto bad;
  if(elf.magic != ELF_MAGIC) goto bad;

  if((pgdir = setupkvm()) == 0) goto bad;

  // 프로그램 로드
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph)) goto bad;
    if(ph.type != ELF_PROG_LOAD) continue;
    if(ph.memsz < ph.filesz) goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr) goto bad;
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0) goto bad;
    if(ph.vaddr % PGSIZE != 0) goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
  }
  iunlockput(ip); end_op(); ip = 0;

  // 스택 2페이지(가드+유저)
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0) goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

  // argv 복사
  for(argc = 0; argv[argc]; argc++){
    if(argc >= MAXARG) goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;
  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer
  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0) goto bad;

  // 프로그램 이름 저장
  for(last=s=path; *s; s++) if(*s == '/') last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // 커밋
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz    = sz;
  curproc->tf->eip = elf.entry;
  curproc->tf->esp = sp;
//  cprintf("[exec] %s: sz=0x%x sp=0x%x\n", last, sz, sp);

  // 새 주소공간으로 하드웨어 전환
  switchuvm(curproc);

  // (A) 소프트 TLB: 같은 PID지만 주소공간이 완전히 바뀌었으므로 전부 무효화
  tlb_invalidate_pid(curproc->pid);

  // (B) 이전 주소공간의 IPT 엔트리 정리 후 메모리 해제
  ipt_scrub_for_pgdir(curproc->pid, oldpgdir);
  freevm(oldpgdir);

  // (C) 새 주소공간을 IPT에 재구축
  ipt_rebuild_for_pgdir(curproc->pid, curproc->pgdir);

  return 0;

bad:
  if(pgdir) freevm(pgdir);
  if(ip){ iunlockput(ip); end_op(); }
  return -1;
}
