//psinfo.c
#include "types.h"
#include "stat.h"
#include "user.h"

static char* s2str(int s){
		// 상태 코드를 문자열로 변환한다
		// 커널이 enum procstate{ } 순서(정수 0~5)에 맞춰 문자열로 바꾼다. 
		switch(s){
				case 0: 
						return "UNUSED";
				case 1: 
						return "EMBRYO";
				case 2:
						return "SLEEPING";
				case 3:
						return "RUNNABLE";
				case 4:
						return "RUNNING";
				case 5: 
						return "ZOMBIE";
		}
		return "UNKNOWN";
}

int main(int argc, char *argv[])
{
		struct procinfo info;
		int pid = (argc >= 2) ? atoi(argv[1]) : 0;     // pid=0이면 자기 자신
		
		// info 구조체에 아래의 5가지 정보를 저장한다
		// 이때 sysproc.c에서 정의했던 get_procinfo() 함수를 사용하면 된다.
		if(get_procinfo(pid, &info) < 0) 
		{
			// 시스템 콜 호출이 실패하면 에러 메시지 후 종료
			// printf의 첫 인자 1은 xv6에서 stdout 파일 디스크립터이다.
			printf(1, "psinfo: failed (pid=%d)\n", pid);
			exit();
		}
		// 지정한 PID의 정보를 한 줄로 출력한다
		printf(1, "PID=%d PPID=%d STATE=%s SZ=%d NAME=%s\n", info.pid, info.ppid, s2str(info.state), info.sz, info.name);
		exit();
}

