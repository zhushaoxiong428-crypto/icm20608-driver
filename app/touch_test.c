#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

/* 应用侧最多跟踪的触摸槽位数（GT911/GT1158 驱动上报 5 个） */
#define TOUCH_MAX_SLOTS 10

/* ddr3分配给屏幕的显存大小为32MiB即：lcd->fix.smem_len */
/* 屏幕一帧画面占用的最大内存容量为 1024*600*4=2.34375MiB */
struct lcd_device {
	int fd;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	unsigned char *memory;
};

static volatile sig_atomic_t running = 1;

/* 参数signo为信号编号，例如Ctrl + C对应的编号为SIGINT */
static void stop_test(int signo)
{
	/* 知道有这个参数，但当前不用它 */
	(void)signo;
	running = 0;
}

static int lcd_clear(struct lcd_device *lcd)
{
	unsigned int y;
	size_t offset;
	size_t bytes_per_pixel = lcd->var.bits_per_pixel / 8;
	size_t bytes = (size_t)lcd->var.xres * bytes_per_pixel;

	for (y = 0; y < lcd->var.yres; y++) {
		offset = ((size_t)y + lcd->var.yoffset) * lcd->fix.line_length +
			(size_t)lcd->var.xoffset * bytes_per_pixel;

		memset(lcd->memory + offset, 0, bytes);
	}

	return 0;
}

static int lcd_close(struct lcd_device *lcd)
{
	int ret = 0;

	if (lcd->memory != MAP_FAILED) {
		if (munmap(lcd->memory, lcd->fix.smem_len) < 0) {
			perror("munmap");
			ret = -1;
		}

		lcd->memory = MAP_FAILED;
	}

	if (lcd->fd >= 0) {
		if (close(lcd->fd) < 0) {
			perror("close framebuffer");
			ret = -1;
		}

		lcd->fd = -1;
	}

	return ret;
}

static int lcd_init(struct lcd_device *lcd)
{
	lcd->fd = open("/dev/fb0", O_RDWR);
	if (lcd->fd < 0) {
		perror("open /dev/fb0");
		return -1;
	}

	/* 解除 LCD 的消隐状态，让屏幕恢复显示 */
	if (ioctl(lcd->fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
		perror("unblank framebuffer");
		goto err;
	}

	if (ioctl(lcd->fd, FBIOGET_VSCREENINFO, &lcd->var) < 0) {
		perror("get variable screen info");
		goto err;
	}

	if (ioctl(lcd->fd, FBIOGET_FSCREENINFO, &lcd->fix) < 0) {
		perror("get fixed screen info");
		goto err;
	}

	/* 将ddr3中framebuffer对应的的物理内存区域映射为应用的虚拟内存空间  */
	lcd->memory = mmap(NULL, lcd->fix.smem_len, PROT_READ | PROT_WRITE,
		MAP_SHARED, lcd->fd, 0);
	if (lcd->memory == MAP_FAILED) {
		perror("mmap framebuffer");
		goto err;
	}

	printf("LCD: %u x %u,%u bpp,line = %u bytes\n", lcd->var.xres,
		lcd->var.yres, lcd->var.bits_per_pixel, lcd->fix.line_length);

	return 0;

err:
	lcd_close(lcd);
	return -1;
}

/*
 * 以(x,y)为中心,画一个7*7像素的蓝色方块
 * 按 framebuffer 实际位深写像素：16 bpp 为 RGB565，32 bpp 为 XRGB8888
 * （原实现固定按 4 字节/像素，在 16 bpp 屏上点会画到 2 倍 x 处并折行）
 */
static void lcd_draw_point(struct lcd_device *lcd, int x, int y)
{
	unsigned int bytes_per_pixel = lcd->var.bits_per_pixel / 8;
	unsigned short pixel16 = 0x001F; /* RGB565 蓝色 */
	unsigned int pixel32 = 0X000000FF;
	const void *pixel = bytes_per_pixel == 2 ? (const void *)&pixel16
						 : (const void *)&pixel32;
	int px;
	int py;
	size_t offset;

	for (py = y - 3; py <= y + 3; py++) {
		for (px = x - 3; px <= x + 3; px++) {
			if (px < 0 || py < 0 || (unsigned int)px >= lcd->var.xres ||
				(unsigned int)py >= lcd->var.yres) {
				continue;
			}

			offset = ((size_t)py + lcd->var.yoffset) * lcd->fix.line_length +
				((size_t)px + lcd->var.xoffset) * bytes_per_pixel;

			memcpy(lcd->memory + offset, pixel, bytes_per_pixel);
		}
	}
}

int main(int argc, char *argv[])
{
	struct lcd_device lcd;
	struct input_event event;
	struct pollfd input_poll;
	struct sigaction
		action; /* 收到某个信号时，要调用谁，处理时额外屏蔽哪些信号，以及使用什么选项。 */
	ssize_t ret;
	int ready;
	int fd = -1;
	int result = EXIT_FAILURE;
	/*
	 * 多点触控协议 B：驱动按槽位(slot)上报，每个槽位对应一根手指。
	 * ABS_X/ABS_Y 只是内核模拟的单点坐标（跟踪最早按下的手指），
	 * 所以这里按槽位保存每根手指的状态，每帧(SYN_REPORT)把所有按下的手指都画出来。
	 */
	int slot = 0;
	int slot_x[TOUCH_MAX_SLOTS];
	int slot_y[TOUCH_MAX_SLOTS];
	int slot_active[TOUCH_MAX_SLOTS];
	int active_count;
	int was_active = 0;
	int i;

	memset(slot_x, 0, sizeof(slot_x));
	memset(slot_y, 0, sizeof(slot_y));
	memset(slot_active, 0, sizeof(slot_active));
	memset(&lcd, 0, sizeof(lcd));
	lcd.fd = -1;
	lcd.memory = MAP_FAILED; /* 指尚未映射 */

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/input/eventX\n", argv[0]);
		return EXIT_FAILURE;
	}

	/* 注册信号处理，不设置 SA_RESTART */
	memset(&action, 0, sizeof(action));
	action.sa_handler = stop_test;
	sigemptyset(&action.sa_mask); /* 不额外添加要阻塞的信号 */

	/* 要处理哪个信号 新的处理规则 不需要取回原来的处理规则*/
	if (sigaction(SIGINT, &action, NULL) < 0 ||
		sigaction(SIGTERM, &action, NULL) < 0) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("open input");
		return EXIT_FAILURE;
	}

	if (lcd_init(&lcd) < 0) {
		goto out;
	}

	memset(&input_poll, 0, sizeof(input_poll));
	input_poll.fd = fd;
	input_poll.events = POLLIN; /* 关心的事件 */

	printf("Poll touch test; Ctrl + C to clear and exit\n");

	while (running) {
		/* 第二个参数为要监视的文件描述符数量 超时200ms后重新检查退出标志 */
		/* 返回值为有事件的文件描述符数量 */
		ready = poll(&input_poll, 1, 200);

		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("poll");
			goto out;
		}

		if (!running) {
			break;
		}

		if (ready == 0) {
			continue;
		}

		if (input_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "Input device unavailable\n");
			goto out;
		}

		if (!(input_poll.revents & POLLIN)) {
			continue;
		}

		while (running) {
			/* 非阻塞读取，没有事件时返回 -1，同时 errno = EAGAIN */
			ret = read(fd, &event, sizeof(event));

			if (ret < 0) {
				if (errno == EINTR) {
					continue;
				}

				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					break;
				}

				perror("read");
				goto out;
			}

			if (ret != (ssize_t)sizeof(event)) {
				fprintf(stderr, "Incomplete input event\n");
				goto out;
			}

			if (!running) {
				break;
			}

			if (event.type == EV_ABS) {
				if (event.code == ABS_MT_SLOT) {
					slot = event.value; /* 后续 ABS_MT_* 事件属于这个槽位 */
				} else if (slot < 0 || slot >= TOUCH_MAX_SLOTS) {
					continue;
				} else if (event.code == ABS_MT_TRACKING_ID) {
					slot_active[slot] = event.value >= 0; /* -1 表示该手指抬起 */
				} else if (event.code == ABS_MT_POSITION_X) {
					slot_x[slot] = event.value;
				} else if (event.code == ABS_MT_POSITION_Y) {
					slot_y[slot] = event.value;
				}
			} else if (event.type == EV_SYN && event.code == SYN_REPORT) {
				active_count = 0;
				for (i = 0; i < TOUCH_MAX_SLOTS; i++) {
					if (!slot_active[i]) {
						continue;
					}
					lcd_draw_point(&lcd, slot_x[i], slot_y[i]);
					printf("TOUCH[%d]: x = %d,y = %d\n", i, slot_x[i], slot_y[i]);
					active_count++;
				}
				if (active_count > 1) {
					printf("POINTS: %d\n", active_count);
				} else if (active_count == 0 && was_active) {
					printf("RELEASED\n");
				}
				was_active = active_count > 0;
			}
		}
	}

	result = EXIT_SUCCESS;

out:
	if (!running && lcd.memory != MAP_FAILED) {
		if (lcd_clear(&lcd) < 0) {
			result = EXIT_FAILURE;
		}
	}

	if (lcd_close(&lcd) < 0) {
		result = EXIT_FAILURE;
	}

	if (close(fd) < 0) {
		perror("close input");
		result = -EXIT_FAILURE;
	}

	return result;
}
