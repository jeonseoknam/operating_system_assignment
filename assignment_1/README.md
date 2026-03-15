**과제 목표**
- xv6 설치 및 컴파일
- 시스템 콜 추가 방법 이해
- 크로스 컴파일 방법 이해


**명세** 
- "Hello xv6 World" 출력하는 "helloxv6" 프로그램을 위한 helloxv6.c 구현
- 프로세스의 상태를 출력하는 "psinfo" 프로그램을 위한 psinfo.c 구현
- xv6에 helloxv6(), get_procinfo() 두 개의 시스템 콜 추가 후 xv6 쉘에서 테스트


**파일 구조**
- user.h: xv6의 시스템 콜 정의
- usys.S: xv6의 시스템 콜 리스트
- syscall.h: 시스템 콜 번호 매핑 
- syscall.c: 시스템 콜 인수를 분석하는 함수 포함 + 시스템 콜에 대한 포인터
- sysproc.c: 프로세스 관련 시스템 콜 구현 -> 여기에 시스템 콜 코드를 추가함
- proc.h: struct proc(프로세스 구조체) 정의 -> 프로세스에 대한 추가 정보 추적을 위해 구조 변경
- proc.c: 프로세스 간의 스케줄링 및 컨텍스트 스위칭을 수행하는 함수

**Demo**
- xv6 내에서 아래 명령어 실행

1. hello_number 시스템 콜 활용 프로그램
```
helloxv6
```
	- helloxv6.c에 작성된 만큼의 hello_number 시스템 콜을 호출하여 결과를 출력하는 프로그램
	- hello_number(int n): 인자 n을 받아, 커널 콘솔에 "Hello, xv6! Your number is n*2"라는 메시지를 출력

2. get_proinfo 시스템 콜 활용 프로그램
```
psinfo [PID]
```
	- [PID]를 입력 받아 해당 process의 정보를 출력
	- 인자가 없을 경우 자신의 pid에 해당하는 process의 정보를 출력
	- PID, PPID, STATE, SIZE, NAME 출력(SIZE=메모리 크기, NAME=프로세스 이름)



