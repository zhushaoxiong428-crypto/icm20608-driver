/* 读 /proc/stat 的中断总数（intr 行第一个数）和上下文切换次数（ctxt 行） */
#ifndef PROCSTAT_H
#define PROCSTAT_H

#include <stdio.h>
#include <string.h>

struct procstat {
	unsigned long long intr, ctxt;
};

static inline void procstat_read(struct procstat *s)
{
	char line[256];
	FILE *f = fopen("/proc/stat", "r");

	s->intr = s->ctxt = 0;
	if (!f)
		return;
	/* intr 行很长（每个中断号一个计数），fgets 只取开头就够了，其余部分按行丢弃 */
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "intr ", 5))
			sscanf(line + 5, "%llu", &s->intr);
		else if (!strncmp(line, "ctxt ", 5))
			sscanf(line + 5, "%llu", &s->ctxt);
		while (!strchr(line, '\n') && fgets(line, sizeof(line), f))
			;
	}
	fclose(f);
}

#endif
