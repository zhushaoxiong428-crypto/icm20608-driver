#include <stdio.h>
/* 程序启动时把 stdout 设为无缓冲，接管道/文件时事件也能立刻输出 */
__attribute__((constructor)) static void stdout_unbuffered(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
}
