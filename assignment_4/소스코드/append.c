// append.c
// 파일 끝에 문자열을 붙이는 도구
// 사용 목적: writei()의 COW 동작 검증하기
// 스냅샷이 잡고 있는 블록을 덮어쓸 때 COW가 제대로 되는지, 간접 블록 영역을 건드릴 때 COW가 잘 되는지 검증하는 용도
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int main(int argc, char *argv[])
{
		// 인자 체크
		// 사용법: append <파일명> "넣으려는 문자열" ex) append hi "hello_snapshot"
		if(argc != 3){
				printf(2, "Usage: append filename string\n");
				exit();
		}

		// open the file for read+write, create if needed
		// 파일이 존재하면 읽기+쓰기로 열고, 없으면 새로 만들고 쓰기 시작
		int fd = open(argv[1], O_RDWR | O_CREATE);
		if(fd < 0){
				printf(2, "append: cannot open %s\n", argv[1]);
				exit();
		}

		// move offset to the end by reading until EOF
		// 1바이트씩 끝까지 읽어서, 파일 오프셋을 EOF 위치로 옮긴다(lseek 대용)
		// 루프가 끝나면 fd의 현재 위치 = 파일 끝.
		char buf[1];
		while(read(fd, buf, 1) == 1);  // drain to EOF
		
		// now at end, write the string
		// 이제 오프셋이 EOF에 있으니, 그대로 쓰면 append 효과가 있다.
		if(write(fd, argv[2], strlen(argv[2])) < 0){
				printf(2, "append: write failed\n");
				close(fd);
				exit();
		}
		close(fd);
		exit();
}
