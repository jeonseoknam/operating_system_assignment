#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]){
		int res = hello_number(5);
		printf(1, "hello_number(5) returned %d\n", res);
		int res2 = hello_number(-7);
	    printf(1, "hello_number(-7) returned %d\n", res2);
		int res3 = hello_number(0);
		printf(1, "hello_number(0) returned %d\n", res3);
		int res4 = hello_number(100000);
		printf(1, "hello_number(100000) returned %d\n", res4);

		exit();
}
