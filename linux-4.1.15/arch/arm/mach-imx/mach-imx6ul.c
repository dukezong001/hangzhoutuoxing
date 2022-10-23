/*
 * Copyright (C) 2015-2016 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fec.h>
#include <linux/gpio.h>
#include <linux/irqchip.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>
#include <linux/netdevice.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <linux/memblock.h>

#include "common.h"
#include "cpuidle.h"
#include "hardware.h"

struct proc_dir_entry *boardname_dir, *boardname_file;  


static void __init imx6ul_enet_clk_init(void)
{
	struct regmap *gpr;

	gpr = syscon_regmap_lookup_by_compatible("fsl,imx6ul-iomuxc-gpr");
	if (!IS_ERR(gpr))
		regmap_update_bits(gpr, IOMUXC_GPR1, IMX6UL_GPR1_ENET_CLK_DIR,
				   IMX6UL_GPR1_ENET_CLK_OUTPUT);
	else
		pr_err("failed to find fsl,imx6ul-iomux-gpr regmap\n");

}

static int ksz8081_phy_fixup(struct phy_device *dev)
{
	return 0;
	if (dev && dev->interface == PHY_INTERFACE_MODE_MII) {
		phy_write(dev, 0x1f, 0x8110);
		phy_write(dev, 0x16, 0x201);
	} else if (dev && dev->interface == PHY_INTERFACE_MODE_RMII) {
		phy_write(dev, 0x1f, 0x8190);
		phy_write(dev, 0x16, 0x202);
	}

	return 0;
}

/*
 * i.MX6UL EVK board RevA, RevB, RevC all use KSZ8081
 * Silicon revision 00, the PHY ID is 0x00221560, pass our
 * test with the phy fixup.
 */
#define PHY_ID_KSZ8081_MNRN60	0x00221560
/*
 * i.MX6UL EVK board RevC1 board use KSZ8081
 * Silicon revision 01, the PHY ID is 0x00221561.
 * This silicon revision still need the phy fixup setting.
 */
#define PHY_ID_KSZ8081_MNRN61	0x00221561
#define PHY_ID_LAN8720_MNRN61 0x0007c0f0
#define PHY_ID_IP101_MNRN61 0x02430c54
static void __init imx6ul_enet_phy_init(void)
{
	//phy_register_fixup(PHY_ANY_ID, PHY_ID_KSZ8081_MNRN60, 0xffffffff, ksz8081_phy_fixup);
	phy_register_fixup(PHY_ANY_ID, PHY_ID_LAN8720_MNRN61, 0xffffffff, ksz8081_phy_fixup);
	//phy_register_fixup(PHY_ANY_ID, PHY_ID_IP101_MNRN61, 0xffffffff, ksz8081_phy_fixup);
}

static int proc_show_ver(struct seq_file *file, void *v)
{
	int cnt = 0;
  
	if(of_machine_is_compatible("MX6UL-C"))
		seq_printf(file, "boardname: %s\n", "MX6UL-C");    
	else if(of_machine_is_compatible("MX6UL-C2"))
		seq_printf(file, "boardname: %s\n", "MX6UL-C2");   
	else if(of_machine_is_compatible("MX6UL-C3"))
		seq_printf(file, "boardname: %s\n", "MX6UL-C3");  
	
	return cnt; 
}
static int proc_boardname_open(struct inode *inode, struct file *file)
{
	single_open(file, proc_show_ver, NULL); 
	return 0;
}

static struct file_operations boardname_fops = {
	.owner   = THIS_MODULE,
	.open    = proc_boardname_open, 
	.read    = seq_read,     
	.llseek  = seq_lseek,
	.release = single_release,  
};

int proc_init(void)
{
	boardname_file = proc_create("boardname", S_IFREG|S_IRUGO, NULL, &boardname_fops);
   	if(boardname_file)
   		return 0;
	else
   		return -ENOMEM;
}


#define OCOTP_CFG3			0x440
#define OCOTP_CFG3_SPEED_SHIFT		16
#define OCOTP_CFG3_SPEED_696MHZ		0x2
#define OCOTP_CFG3_SPEED_1_GHZ		0x3

static void __init imx6ul_opp_check_speed_grading(struct device *cpu_dev)
{
	struct device_node *np;
	void __iomem *base;
	u32 val;

	if (cpu_is_imx6ul())
		np = of_find_compatible_node(NULL, NULL, "fsl,imx6ul-ocotp");
	else
		np = of_find_compatible_node(NULL, NULL, "fsl,imx6ull-ocotp");

	if (!np) {
		pr_warn("failed to find ocotp node\n");
		return;
	}

	base = of_iomap(np, 0);
	if (!base) {
		pr_warn("failed to map ocotp\n");
		goto put_node;
	}

	/*
	 * Speed GRADING[1:0] defines the max speed of ARM:
	 * 2b'00: Reserved;
	 * 2b'01: 528000000Hz;
	 * 2b'10: 700000000Hz(i.MX6UL), 800000000Hz(i.MX6ULL);
	 * 2b'11: Reserved(i.MX6UL), 1GHz(i.MX6ULL);
	 * We need to set the max speed of ARM according to fuse map.
	 */
	val = readl_relaxed(base + OCOTP_CFG3);
	val >>= OCOTP_CFG3_SPEED_SHIFT;
	val &= 0x3;
	if (cpu_is_imx6ul()) {
		if (val < OCOTP_CFG3_SPEED_696MHZ) {
			if (dev_pm_opp_disable(cpu_dev, 696000000))
				pr_warn("Failed to disable 696MHz OPP\n");
		}
	}

	if (cpu_is_imx6ull()) {
		if (val != OCOTP_CFG3_SPEED_1_GHZ) {
			if (dev_pm_opp_disable(cpu_dev, 996000000))
				pr_warn("Failed to disable 996MHz OPP\n");
		}

		if (val != OCOTP_CFG3_SPEED_696MHZ) {
			if (dev_pm_opp_disable(cpu_dev, 792000000))
				pr_warn("Failed to disable 792MHz OPP\n");
		}
	}
	iounmap(base);

put_node:
	of_node_put(np);
}

static void __init imx6ul_opp_init(void)
{
	struct device_node *np;
	struct device *cpu_dev = get_cpu_device(0);
//printk("111111111111111111111111111111111111111111111\n");
	if (!cpu_dev) {
		pr_warn("failed to get cpu0 device\n");
		return;
	}
	np = of_node_get(cpu_dev->of_node);
	if (!np) {
		pr_warn("failed to find cpu0 node\n");
		return;
	}

	if (of_init_opp_table(cpu_dev)) {
		pr_warn("failed to init OPP table\n");
		goto put_node;
	}

	imx6ul_opp_check_speed_grading(cpu_dev);

put_node:
	of_node_put(np);
}

static inline void imx6ul_enet_init(void)
{
	//printk("222222222222222222222222222222222222222222\n");
	//gpio_request(IMX_GPIO_NR(2, 18) ,NULL);
	//while(1)
	{
		
	}
	imx6ul_enet_clk_init();
	imx6ul_enet_phy_init();
	if (cpu_is_imx6ul())
		imx6_enet_mac_init("fsl,imx6ul-fec", "fsl,imx6ul-ocotp");
	else
		imx6_enet_mac_init("fsl,imx6ul-fec", "fsl,imx6ull-ocotp");

}

static void __init imx6ul_init_machine(void)
{
	struct device *parent;
	void __iomem *iomux;
	struct device_node *np;	

	parent = imx_soc_device_init();
	//printk("33333ffffffffffffffffffffffffffffffffffffff333333333\n");
	if (parent == NULL)
		pr_warn("failed to initialize soc device\n");

	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
	imx6ul_enet_init();
	imx_anatop_init();
	imx6ul_pm_init();
	
		          np = of_find_compatible_node(NULL,NULL,"fsl,imx6ul-iomuxc");
              iomux = of_iomap(np, 0);//找到iomux寄存器基地址
              writel_relaxed(0x2,iomux+0x650); 
			  writel_relaxed(0x2,iomux+0x658); 
			  
		//gpio_direction_output(IMX_GPIO_NR(2, 18) , 0);
	//mdelay(1);
	//gpio_direction_output(IMX_GPIO_NR(2, 18) , 1);
	//mdelay(1);
	//gpio_direction_output(IMX_GPIO_NR(2, 18) , 0);
	//mdelay(1);
	//gpio_direction_output(IMX_GPIO_NR(2, 18) , 1);
	//mdelay(1);
		//gpio_direction_output(IMX_GPIO_NR(2, 18) , 0);
	//mdelay(1);
	//gpio_direction_output(IMX_GPIO_NR(2, 18) , 1);
	//mdelay(1);
//printk("44444444444444444444444444444444444444444444444444\n");
	//np = of_find_compatible_node(NULL,NULL,"fsl,imx6ul-iomuxc");
     //   iomux = of_iomap(np, 0);
    //    writel_relaxed(0x2,iomux+0x650);
    //    writel_relaxed(0x3,iomux+0x658);
	//proc_init();
}

static void __init imx6ul_init_irq(void)
{
	//printk("6666666666666666666666666666666666\n");
	imx_gpc_check_dt();
	imx_init_revision_from_anatop();
	imx_src_init();
	irqchip_init();
}

static void __init imx6ul_init_late(void)
{//printk("555555555555555555555555555555555555555\n");
	if (IS_ENABLED(CONFIG_ARM_IMX6Q_CPUFREQ)) {
		imx6ul_opp_init();
		platform_device_register_simple("imx6q-cpufreq", -1, NULL, 0);
	}

	imx6ul_cpuidle_init();

}

static void __init imx6ul_map_io(void)
{
	debug_ll_io_init();
	imx6_pm_map_io();
	imx_busfreq_map_io();
}

#ifdef CONFIG_MX6_CLK_FOR_BOOTUI_TRANS
static void imx6ul_init_reserve(void)
{
	phys_addr_t base, size;

	/*
	 * Frame buffer base address.
	 * It is same as CONFIG_FB_BASE in Uboot.
	 */
	base = 0x87b00000;

	/*
	 * Reserved display memory size.
	 * It should be bigger than 3 x framer buffer size.
	 * For 1080P 32 bpp, 1920*1080*4*3 = 0x017BB000.
	 */
	size = 0x01000000;

	memblock_reserve(base, size);
	memblock_remove(base, size);
}
#endif 


static const char *imx6ul_dt_compat[] __initconst = {
	"fsl,imx6ul",
	"fsl,imx6ull",
	NULL,
};

DT_MACHINE_START(IMX6UL, "Freescale i.MX6 Ultralite (Device Tree)")
	.map_io		= imx6ul_map_io,
	.init_irq	= imx6ul_init_irq,
	.init_machine	= imx6ul_init_machine,
	.init_late	= imx6ul_init_late,
	.dt_compat	= imx6ul_dt_compat,
#ifdef CONFIG_MX6_CLK_FOR_BOOTUI_TRANS
	.reserve        = imx6ul_init_reserve,
#endif
MACHINE_END
