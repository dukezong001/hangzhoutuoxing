/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <autoboot.h>
#include <bootretry.h>
#include <cli.h>
#include <console.h>
#include <fdtdec.h>
#include <menu.h>
#include <post.h>
#include <u-boot/sha256.h>
#include <asm/gpio.h>


#include <asm/arch/clock.h>
#include <asm/arch/iomux.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/crm_regs.h>
#include <asm/arch/mx6ull_pins.h>
#include <asm/arch/mx6-pins.h>
#include <asm/arch/sys_proto.h>
#include <asm/gpio.h>
#include <asm/imx-common/iomux-v3.h>
#include <asm/imx-common/boot_mode.h>
#include <asm/imx-common/mxc_i2c.h>
#include <asm/io.h>
#include <common.h>
#include <fsl_esdhc.h>
#include <i2c.h>
#include <miiphy.h>
#include <linux/sizes.h>
#include <mmc.h>
#include <mxsfb.h>
#include <netdev.h>
#include <power/pmic.h>
#include <power/pfuze3000_pmic.h>
#include <usb.h>
#include <usb/ehci-fsl.h>
#include <asm/imx-common/video.h>


#ifdef is_boot_from_usb
#include <environment.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

#define MAX_DELAY_STOP_STR 32

#ifndef DEBUG_BOOTKEYS
#define DEBUG_BOOTKEYS 0
#endif
#define debug_bootkeys(fmt, args...)		\
	debug_cond(DEBUG_BOOTKEYS, fmt, ##args)

/* Stored value of bootdelay, used by autoboot_command() */
static int stored_bootdelay;

#if defined(CONFIG_AUTOBOOT_KEYED)
#if defined(CONFIG_AUTOBOOT_STOP_STR_SHA256)

/*
 * Use a "constant-length" time compare function for this
 * hash compare:
 *
 * https://crackstation.net/hashing-security.htm
 */
static int slow_equals(u8 *a, u8 *b, int len)
{
	int diff = 0;
	int i;

	for (i = 0; i < len; i++)
		diff |= a[i] ^ b[i];

	return diff == 0;
}

static int passwd_abort(uint64_t etime)
{
	const char *sha_env_str = getenv("bootstopkeysha256");
	u8 sha_env[SHA256_SUM_LEN];
	u8 sha[SHA256_SUM_LEN];
	char presskey[MAX_DELAY_STOP_STR];
	const char *algo_name = "sha256";
	u_int presskey_len = 0;
	int abort = 0;
	int size;
	int ret;

	if (sha_env_str == NULL)
		sha_env_str = CONFIG_AUTOBOOT_STOP_STR_SHA256;

	/*
	 * Generate the binary value from the environment hash value
	 * so that we can compare this value with the computed hash
	 * from the user input
	 */
	ret = hash_parse_string(algo_name, sha_env_str, sha_env);
	if (ret) {
		printf("Hash %s not supported!\n", algo_name);
		return 0;
	}

	/*
	 * We don't know how long the stop-string is, so we need to
	 * generate the sha256 hash upon each input character and
	 * compare the value with the one saved in the environment
	 */
	do {
		if (tstc()) {
			/* Check for input string overflow */
			if (presskey_len >= MAX_DELAY_STOP_STR)
				return 0;

			presskey[presskey_len++] = getc();

			/* Calculate sha256 upon each new char */
			hash_block(algo_name, (const void *)presskey,
				   presskey_len, sha, &size);

			/* And check if sha matches saved value in env */
			if (slow_equals(sha, sha_env, SHA256_SUM_LEN))
				abort = 1;
		}
	} while (!abort && get_ticks() <= etime);

	return abort;
}
#else
static int passwd_abort(uint64_t etime)
{
	int abort = 0;
	struct {
		char *str;
		u_int len;
		int retry;
	}
	delaykey[] = {
		{ .str = getenv("bootdelaykey"),  .retry = 1 },
		{ .str = getenv("bootstopkey"),   .retry = 0 },
	};

	char presskey[MAX_DELAY_STOP_STR];
	u_int presskey_len = 0;
	u_int presskey_max = 0;
	u_int i;

#  ifdef CONFIG_AUTOBOOT_DELAY_STR
	if (delaykey[0].str == NULL)
		delaykey[0].str = CONFIG_AUTOBOOT_DELAY_STR;
#  endif
#  ifdef CONFIG_AUTOBOOT_STOP_STR
	if (delaykey[1].str == NULL)
		delaykey[1].str = CONFIG_AUTOBOOT_STOP_STR;
#  endif

	for (i = 0; i < sizeof(delaykey) / sizeof(delaykey[0]); i++) {
		delaykey[i].len = delaykey[i].str == NULL ?
				    0 : strlen(delaykey[i].str);
		delaykey[i].len = delaykey[i].len > MAX_DELAY_STOP_STR ?
				    MAX_DELAY_STOP_STR : delaykey[i].len;

		presskey_max = presskey_max > delaykey[i].len ?
				    presskey_max : delaykey[i].len;

		debug_bootkeys("%s key:<%s>\n",
			       delaykey[i].retry ? "delay" : "stop",
			       delaykey[i].str ? delaykey[i].str : "NULL");
	}

	/* In order to keep up with incoming data, check timeout only
	 * when catch up.
	 */
	do {
		if (tstc()) {
			if (presskey_len < presskey_max) {
				presskey[presskey_len++] = getc();
			} else {
				for (i = 0; i < presskey_max - 1; i++)
					presskey[i] = presskey[i + 1];

				presskey[i] = getc();
			}
		}

		for (i = 0; i < sizeof(delaykey) / sizeof(delaykey[0]); i++) {
			if (delaykey[i].len > 0 &&
			    presskey_len >= delaykey[i].len &&
				memcmp(presskey + presskey_len -
					delaykey[i].len, delaykey[i].str,
					delaykey[i].len) == 0) {
					debug_bootkeys("got %skey\n",
						delaykey[i].retry ? "delay" :
						"stop");

				/* don't retry auto boot */
				if (!delaykey[i].retry)
					bootretry_dont_retry();
				abort = 1;
			}
		}
	} while (!abort && get_ticks() <= etime);

	return abort;
}
#endif

/***************************************************************************
 * Watch for 'delay' seconds for autoboot stop or autoboot delay string.
 * returns: 0 -  no key string, allow autoboot 1 - got key string, abort
 */
static int abortboot_keyed(int bootdelay)
{
	int abort;
	uint64_t etime = endtick(bootdelay);

#ifndef CONFIG_ZERO_BOOTDELAY_CHECK
	if (bootdelay == 0)
		return 0;
#endif

#  ifdef CONFIG_AUTOBOOT_PROMPT
	/*
	 * CONFIG_AUTOBOOT_PROMPT includes the %d for all boards.
	 * To print the bootdelay value upon bootup.
	 */
	printf(CONFIG_AUTOBOOT_PROMPT, bootdelay);
#  endif

	abort = passwd_abort(etime);
	if (!abort)
		debug_bootkeys("key timeout\n");

#ifdef CONFIG_SILENT_CONSOLE
	if (abort)
		gd->flags &= ~GD_FLG_SILENT;
#endif

	return abort;
}

# else	/* !defined(CONFIG_AUTOBOOT_KEYED) */

#ifdef CONFIG_MENUKEY
static int menukey;
#endif
static iomux_v3_cfg_t const iox_pads[] = {
	/* IOX_SDI */
	MX6_PAD_BOOT_MODE0__GPIO5_IO10 | MUX_PAD_CTRL(NO_PAD_CTRL),
	/* IOX_SHCP */
	MX6_PAD_BOOT_MODE1__GPIO5_IO11 | MUX_PAD_CTRL(NO_PAD_CTRL),
	/* IOX_STCP */
	MX6_PAD_SNVS_TAMPER7__GPIO5_IO07 | MUX_PAD_CTRL(NO_PAD_CTRL),
	/* IOX_nOE */
	MX6_PAD_SNVS_TAMPER8__GPIO5_IO08 | MUX_PAD_CTRL(NO_PAD_CTRL),
	MX6_PAD_SD1_DATA0__GPIO2_IO18 | MUX_PAD_CTRL(NO_PAD_CTRL),
};
static int abortboot_normal(int bootdelay)
{
	int abort = 0;
	unsigned long ts;

#ifdef CONFIG_MENUPROMPT
	printf(CONFIG_MENUPROMPT);
#else
	if (bootdelay >= 0)
		printf("Hit any key to stop autoboot jhf: %2d ", bootdelay);
#endif

#if defined CONFIG_ZERO_BOOTDELAY_CHECK
	/*
	 * Check if key already pressed
	 * Don't check if bootdelay < 0
	 */
	if (bootdelay >= 0) {
		if (tstc()) {	/* we got a key press	*/
			(void) getc();  /* consume input	*/
			puts("\b\b\b 0");
			abort = 1;	/* don't auto boot	*/
		}
	}
#endif
imx_iomux_v3_setup_multiple_pads(iox_pads, ARRAY_SIZE(iox_pads));
//gpio_request(IMX_GPIO_NR(2, 18) ,NULL);
	while ((bootdelay > 0) && (!abort)) {
		--bootdelay;
		/* delay 1000 ms */
		ts = get_timer(0);
		do {
			if (tstc()) {	/* we got a key press	*/
				abort  = 1;	/* don't auto boot	*/
				bootdelay = 0;	/* no more delay	*/
# ifdef CONFIG_MENUKEY
//printf("hhhhhhhhhhhhhhhhhhh\n");
				menukey = getc();
				//printf("ppppppppppppppppppp\n");
# else
	//printf("ggggggggggggggggggggggggggg\n");
				(void) getc();  /* consume input	*/
				//printf("ffffffffffffffffffffffffffff\n");
# endif
				break;
			}
			//while(1)
			{
			//gpio_direction_output(IMX_GPIO_NR(2, 18) , 0);
			//udelay(3000);
			//gpio_direction_output(IMX_GPIO_NR(2, 18) , 1);
			//udelay(3000);
			}
			udelay(10000);
		} while (!abort && get_timer(ts) < 1000);
		//while(1){
   // gpio_direction_output(IMX_GPIO_NR(2, 18) , 0);
	//mdelay(1);
	//gpio_direction_output(IMX_GPIO_NR(2, 18) , 1);
	//mdelay(1);
	//gpio_direction_output(IMX_GPIO_NR(2, 18) , 0);
	//mdelay(1);
	//gpio_direction_output(IMX_GPIO_NR(2, 18) , 1);
	mdelay(1);
	//	}
		printf("\b\b\b%2d ", bootdelay);
	}

	putc('\n');

#ifdef CONFIG_SILENT_CONSOLE
	if (abort)
		gd->flags &= ~GD_FLG_SILENT;
#endif
//printf("666666666666666++++++++++++++++++++++\n");
	return abort;
}
# endif	/* CONFIG_AUTOBOOT_KEYED */

static int abortboot(int bootdelay)
{
#ifdef CONFIG_AUTOBOOT_KEYED
	return abortboot_keyed(bootdelay);
#else
	return abortboot_normal(bootdelay);
#endif
}

static void process_fdt_options(const void *blob)
{
#if defined(CONFIG_OF_CONTROL) && defined(CONFIG_SYS_TEXT_BASE)
	ulong addr;

	/* Add an env variable to point to a kernel payload, if available */
	addr = fdtdec_get_config_int(gd->fdt_blob, "kernel-offset", 0);
	if (addr)
		setenv_addr("kernaddr", (void *)(CONFIG_SYS_TEXT_BASE + addr));

	/* Add an env variable to point to a root disk, if available */
	addr = fdtdec_get_config_int(gd->fdt_blob, "rootdisk-offset", 0);
	if (addr)
		setenv_addr("rootaddr", (void *)(CONFIG_SYS_TEXT_BASE + addr));
#endif /* CONFIG_OF_CONTROL && CONFIG_SYS_TEXT_BASE */
}

const char *bootdelay_process(void)
{
	char *s;
	int bootdelay;
#ifdef CONFIG_BOOTCOUNT_LIMIT
	unsigned long bootcount = 0;
	unsigned long bootlimit = 0;
#endif /* CONFIG_BOOTCOUNT_LIMIT */

#ifdef CONFIG_BOOTCOUNT_LIMIT
	bootcount = bootcount_load();
	bootcount++;
	bootcount_store(bootcount);
	setenv_ulong("bootcount", bootcount);
	bootlimit = getenv_ulong("bootlimit", 10, 0);
#endif /* CONFIG_BOOTCOUNT_LIMIT */

	s = getenv("bootdelay");
	bootdelay = s ? (int)simple_strtol(s, NULL, 10) : CONFIG_BOOTDELAY;

#if !defined(CONFIG_FSL_FASTBOOT) && defined(is_boot_from_usb)
	if (is_boot_from_usb()) {
		disconnect_from_pc();
		printf("Boot from USB for mfgtools\n");
		bootdelay = 0;
		set_default_env("Use default environment for \
				 mfgtools\n");
	} else {
		printf("Normal Boot\n");
	}
#endif

#ifdef CONFIG_OF_CONTROL
	bootdelay = fdtdec_get_config_int(gd->fdt_blob, "bootdelay",
			bootdelay);
#endif

	debug("### main_loop entered: bootdelay=%d\n\n", bootdelay);

#if defined(CONFIG_MENU_SHOW)
	bootdelay = menu_show(bootdelay);
#endif
	bootretry_init_cmd_timeout();

#ifdef CONFIG_POST
	if (gd->flags & GD_FLG_POSTFAIL) {
		s = getenv("failbootcmd");
	} else
#endif /* CONFIG_POST */
#ifdef CONFIG_BOOTCOUNT_LIMIT
	if (bootlimit && (bootcount > bootlimit)) {
		printf("Warning: Bootlimit (%u) exceeded. Using altbootcmd.\n",
		       (unsigned)bootlimit);
		s = getenv("altbootcmd");
	} else
#endif /* CONFIG_BOOTCOUNT_LIMIT */
		s = getenv("bootcmd");

#if !defined(CONFIG_FSL_FASTBOOT) && defined(is_boot_from_usb)
	if (is_boot_from_usb()) {
		s = getenv("bootcmd_mfg");
		printf("Run bootcmd_mfg: %s\n", s);
	}
#endif

	process_fdt_options(gd->fdt_blob);
	stored_bootdelay = bootdelay;

	return s;
}

void autoboot_command(const char *s)
{
	debug("### main_loop: bootcmd=\"%s\"\n", s ? s : "<UNDEFINED>");
//printf("tttttttt//???????????????????????????????????ttt\n");
	if (stored_bootdelay != -1 && s && !abortboot(stored_bootdelay)) {
		//printf("tttttttt//////////////////////////tttttttt\n");
#if defined(CONFIG_AUTOBOOT_KEYED) && !defined(CONFIG_AUTOBOOT_KEYED_CTRLC)
		int prev = disable_ctrlc(1);	/* disable Control C checking */
#endif
//printf("ttttttttttttttttttttttttttttt\n");
		run_command_list(s, -1, 0);
//printf("uuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuu\n");
#if defined(CONFIG_AUTOBOOT_KEYED) && !defined(CONFIG_AUTOBOOT_KEYED_CTRLC)
//printf("55555555555555555555555555\n");
		disable_ctrlc(prev);	/* restore Control C checking */
		//printf("9999999999999999999999999999\n");
#endif
	}

#ifdef CONFIG_MENUKEY
//printf("77777777777777777777777777777777777\n");
	if (menukey == CONFIG_MENUKEY) {
		s = getenv("menucmd");
		if (s)
			run_command_list(s, -1, 0);
	}
#endif /* CONFIG_MENUKEY */
}
