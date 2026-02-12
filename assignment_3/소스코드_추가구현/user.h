#include "physmem.h"
#include "vlist.h"

struct stat;
struct rtcdate;

// system calls
int fork(void);
int exit(void) __attribute__((noreturn));
int wait(void);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
int sleep(int);
int uptime(void);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void printf(int, const char*, ...);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
void* malloc(uint);
void free(void*);
int atoi(const char*);

//#include "physmem.h"  // 유저 프로그램, 커널이 모두 같은 구조체 사용하도록 공용 헤더 추가
int dump_physmem_info(void *addr, int max_entries);  // 시스템콜 프로토타입

// 가상주소 → 물리주소 변환(sw_vtop 커널 함수 기반 사용자 버전)
int vtop(void *va, uint32_t *pa_out, uint32_t *flags_out);


// 소프트웨어 TLB 관련 구조체 및 시스템콜 정의
struct tlb_stats {
  uint32_t hits, misses, evicts, invalidations, lookups;  // 동작 통계
  uint32_t lines, ways, sets;                             // TLB 구성 파라미터
};

struct tlb_entry;  // 내부 구조체 참조용(정의는 커널측 tlb.h에 존재)

// 소프트웨어 TLB 통계 조회 시스템콜
int tlb_stats_sys(struct tlb_stats *out);

// 소프트웨어 TLB 리셋: mode=0(카운터만 초기화), 1(엔트리 포함 전체 초기화)
int sw_tlb_reset(int mode);

// 소프트웨어 TLB 현재 상태를 읽어오는 시스템콜
int sw_tlb_read(struct tlb_stats *out);


// 물리 ↔ 가상 매핑 관련 시스템콜
int phys2virt(uint pa_page, struct vlist *out, int max);    // 물리 페이지 → (pid, va, flags) 역조회
int vm_alias_by_pa(uint pa_page, void *dst_va, uint flags); // 동일 물리 페이지를 새로운 VA에 alias 매핑
int vm_unmap_one(void *va, int free_if_last);               // 지정 VA 매핑 해제 (마지막 참조면 프레임 해제)
int vm_map_user_page(uint va_page, uint pa_page, int perm); // 지정 VA에 물리 페이지 수동 매핑
int protect_nowrite(void *va);                              // VA의 쓰기 권한 제거(RO 보호)