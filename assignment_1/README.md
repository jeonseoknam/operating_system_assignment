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


