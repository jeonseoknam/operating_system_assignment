**과제 이름**
- Stride Scheduling in xv6

**과제 목표**
- 타겟 개수에 비례하여 CPU 시간을 할당하는 결정론적 stride 스케줄러를 xv6에 구현
- 기존의 우선순위나 가중치 기반 스케줄링과는 다른 방식으로 CPU 분배를 달성하고, 스케줄러를 변경하는 과정 학습
- 커널 수준에서 스케줄링 알고리즘 구현 방법, 프로세스 구조체 확장 연습
- 스케줄링 기법의 차이점 이해
- 시스템 콜 추가 방법 복습

**스케줄링이란**
- OS가 제한된 시스템 자원(cpu 등)을 여러 프로세스에 효율적이고 공정하게 할당하기 위해 작업의 진행 순서와 시간을 정하는 기술
- cpu 이용률을 최대화하고, 대기 시간과 응답 시간을 줄이는 것이 목적
- 리눅스: CFS(Completely Fair Scheduler)를 기본 스케줄러로 사용한다
- xv6: Round Robin을 기본 스케줄러로 사용한다

- 작동 방식:
	- 비선점형(non-preemptive): 한 프로세스가 cpu를 차지하면 작업이 끝날 때까지 뺏기지 않음
	- 선점형(preemptive): 우선순위가 높은 프로세스가 현재 cpu 사용 중인 프로세스를 멈추고 자원을 뺏을 수 있음(예: Round Robin)

**Demo**
- [assignment_4 description](https://flat-spot-421.notion.site/assignment_2-324158dedd6080ed91faf01b1cad4158?source=copy_link)
