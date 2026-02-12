// mk_test_file.c
// 테스트용으로 직접 블록 + 간접 블록까지 한번에 테스트 할 수 있는 큰 파일 만드는 사용자 프로그램
// 사용법: mk_test_file <파일명> 으로 실행하면 된다
// 생성되는 파일 구조:
// 블록0: "0 0 0 ... 0 \n"
// 블록1: "1 0 0 ... 0 \n"
// 블록11: "1 0 0 ... 0 \n"
// 그 뒤에 "hello\n"이 간접 블록으로 이어짐

#include "types.h"
#include "fcntl.h"
#include "user.h"

int main(int argc, char *argv[]){
		int fd;
		// 인자 없이 사용하면 종료
		if(argc < 2){
				printf(1, "need argv[1]\n");
				exit();
		}
		// 잘못된 open이면 종료
		if((fd = open(argv[1], O_CREATE | O_WRONLY)) < 0){
				printf(1, "open error for %s\n", argv[0]);
				exit();
		}
		char buf[513];
		// buf[0]: '0'~'9' 같은 숫자 문자(블록 번호용)로 채운다
		// buf[1..510]: 전부 0으로 채운다
		// buf[511]: '\n'
		for(int i = 1; i < 511; i++)
				buf[i] = 0;
		buf[511] = '\n';

		// xv6의 NDIRECT 값은 기본적으로 12라서, 이 루프는 파일의 직접 블록 12개를 정확히 채우는 역할을 한다
		// i=0 -> '0', i=1 ->'1', ... i=10 -> '0', i=11 -> '1'
		for(int i=0; i < 12; i++){
				buf[0] = i % 10 + '0';
				write(fd, buf, 512);
		}
		// 이미 위에서 12개의 블록(12*512바이트)을 써서, 파일 오프셋은 정확히 12*512 바이트 지점이다.
		// 이제는 간접 블록 영역이 시작되는 위치이다.
		// 직접 블록이 다 채워지면 그 뒤에 hello를 쓴다. 이는 간접 블록 영역에 작성된다
		// 즉 간접 블록에도 COW가 잘 되는지까지 한번에 테스트하는 용도이다.
		char *str = "hello\n";
		write(fd, str, 6);
		close(fd);
		exit();
}

