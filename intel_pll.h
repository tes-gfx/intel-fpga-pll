#ifndef __ALTERA_PLL_H__
#define __ALTERA_PLL_H__

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>


struct altera_pll {
  struct clk_hw hw;

  struct device *dev;
  unsigned long rate;
  void __iomem  *mmio;
};

#define to_altera_pll(p) container_of(p, struct altera_pll, hw)

#endif /* __ALTERA_PLL_H__ */
