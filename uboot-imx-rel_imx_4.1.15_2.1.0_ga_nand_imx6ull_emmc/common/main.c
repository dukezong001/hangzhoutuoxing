/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/* #define	DEBUG	*/

#include <common.h>
#include <autoboot.h>
#include <cli.h>
#include <console.h>
#include <version.h>
#include <asm/gpio.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * Board-specific Platform code can reimplement show_boot_progress () if needed
 */
__weak void show_boot_progress(int val) {}

static void run_preboot_environment_command(void)
{
#ifdef CONFIG_PREBOOT
	char *p;

	p = getenv("preboot");
	if (p != NULL) {
# ifdef CONFIG_AUTOBOOT_KEYED
		int prev = disable_ctrlc(1);	/* disable Control C checking */
# endif

		run_command_list(p, -1, 0);

# ifdef CONFIG_AUTOBOOT_KEYED
		disable_ctrlc(prev);	/* restore Control C checking */
# endif
	}
#endif /* CONFIG_PREBOOT */
}

#define UBOOT_NAME ("u-boot.imx")
#define KERNEL_NAME ("zimage")
#define DTB_NAME ("b-imx6ull4igwemmc-emmc.dtb")
#define ROOTFS_NAME ("rootfs.ext4")

static void update_file(int mmc_dev, int mmc_part, uint64_t addr, 
								uint64_t mmc_blk, uint64_t mmc_cnt, char *filename)
{
	int ret = 0;
	int i = 0;
	char command[64] = {0};

	memset(command, 0, 64);
	sprintf(command, "fatload usb 0 0x%llx %s", addr, filename);
	for (i = 0; i < 10; i++) {
		ret = run_command(command, 0);
		if (0 == ret)
			break;
	}
	if (i >= 10) {
		printf("[%s %d] %s failed.", __FUNCTION__, __LINE__, command);
		// 升级失败则灭灯
		gpio_direction_output(IMX_GPIO_NR(1, 4) , 1);
		return;
	}

	memset(command, 0, 64);
	sprintf(command, "mmc dev %d %d", mmc_dev, mmc_part);
	for (i = 0; i < 10; i++) {
		ret = run_command(command, 0);
		if (0 == ret)
			break;
	}
	if (i >= 10) {
		printf("[%s %d] %s failed.", __FUNCTION__, __LINE__, command);
		gpio_direction_output(IMX_GPIO_NR(1, 4) , 1);
		return;
	}

	memset(command, 0, 64);
	sprintf(command, "mmc write 0x%llx 0x%llx 0x%llx", addr, mmc_blk, mmc_cnt);
	for (i = 0; i < 10; i++) {
		ret = run_command(command, 0);
		if (0 == ret)
			break;
	}
	if (i >= 10) {
		printf("[%s %d] %s failed.", __FUNCTION__, __LINE__, command);
		gpio_direction_output(IMX_GPIO_NR(1, 4) , 1);
		return;
	}
}

static void update_by_usb(void)
{
	int i = 0;
	int ret = 0;
	char command[64] = {0};

	gpio_request(IMX_GPIO_NR(1, 4), NULL);

	/* 左下角灯，刚开始快闪5次，后常亮 */
	for (i = 0; i < 5; i++)
	{
		gpio_direction_output(IMX_GPIO_NR(1, 4) , 0);
		mdelay(100);
		gpio_direction_output(IMX_GPIO_NR(1, 4) , 1);
		mdelay(100);
	}
	gpio_direction_output(IMX_GPIO_NR(1, 4) , 0);

	// usb start
	memset(command, 0, 64);
	sprintf(command, "usb start");
	ret = run_command(command, 0);
	if (0 != ret)
	{
		printf("[%s %d] usb start failed.", __FUNCTION__, __LINE__);
	}

	for (i = 0; i < 10; i++)
	{
		memset(command, 0, 64);
		sprintf(command, "fatls usb 0");
		ret = run_command(command, 0);
		if (0 == ret) {
			printf("============update uboot=============\n");
			update_file(1, 1, 0x80800000, 0x02, 0x800, UBOOT_NAME);
			
			printf("============update kernel=============\n");
			update_file(1, 0, 0x80800000, 0x8000, 0x5000, KERNEL_NAME);
			
			printf("============update dtb=============\n");
			update_file(1, 0, 0x80800000, 0x28000, 0x800, DTB_NAME);
			
			printf("============update rootfs=============\n");
			update_file(1, 0, 0x80800000, 0x64000, 0x28000, ROOTFS_NAME);

			break;
		}
	}

	if (i >= 10) {
		/* 如果没有U盘 则灭灯 */
		gpio_direction_output(IMX_GPIO_NR(1, 4) , 1);
		return;
	}

	/* 升级完成后慢闪10次 */
	for (i = 0; i < 10; i++)
	{
		gpio_direction_output(IMX_GPIO_NR(1, 4) , 0);
		mdelay(500);
		gpio_direction_output(IMX_GPIO_NR(1, 4) , 1);
		mdelay(500);
	}
}

/* We come here after U-Boot is initialised and ready to process commands */
void main_loop(void)
{
	const char *s;

	bootstage_mark_name(BOOTSTAGE_ID_MAIN_LOOP, "main_loop");

#ifndef CONFIG_SYS_GENERIC_BOARD
	puts("Warning: Your board does not use generic board. Please read\n");
	puts("doc/README.generic-board and take action. Boards not\n");
	puts("upgraded by the late 2014 may break or be removed.\n");
#endif

#ifdef CONFIG_VERSION_VARIABLE
	setenv("ver", version_string);  /* set version variable */
#endif /* CONFIG_VERSION_VARIABLE */

	cli_init();

	run_preboot_environment_command();

#if defined(CONFIG_UPDATE_TFTP)
	update_tftp(0UL, NULL, NULL);
#endif /* CONFIG_UPDATE_TFTP */

	s = bootdelay_process();
	if (cli_process_fdt(&s))
		cli_secure_boot_cmd(s);
//puts("1111111111111111111111111111111111111.\n");

	update_by_usb();

	autoboot_command(s);
//puts("2222222222222222222222222222222222222.\n");
	cli_loop();
}
