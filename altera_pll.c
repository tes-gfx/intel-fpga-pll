#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/bug.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>

#include "altera_pll.h"


/* Register defines */
#define ALTERA_PLL_REGIDX_MODE        (0)
#define ALTERA_PLL_REGIDX_START       (2)
#define ALTERA_PLL_REGIDX_COUNT_N     (3)
#define ALTERA_PLL_REGIDX_COUNT_M     (4)
#define ALTERA_PLL_REGIDX_COUNT_C     (5)
#define ALTERA_PLL_REGIDX_BANDWITH    (8)
#define ALTERA_PLL_REGIDX_CHARGE_PUMP (9)

/* Register masks */
#define COUNTER_BYPASS_ENABLE         (1<<16)
#define COUNTER_ODD_DIVIDE_ENABLE     (1<<17)

struct pll_descr {
	u32 f_in;  // in hz (reference clock)
	u32 f_out; // out hz
	u32 m;
	u32 n;
	u32 c;
};

struct pll_config {
	unsigned long rate;
	u32 count_m;
	u32 count_n;
	u32 count_c;
	u32 bandwith;
	u32 charge_pump;
};

u32 to_counter_reg(u32 count)
{
	u32 res = count / 2;

	if(count & 0x1) {
		res |= (res + 1) << 8;
		res |= COUNTER_ODD_DIVIDE_ENABLE;
	}
	else
		res |= res << 8;

	return res;
}

struct pll_config to_pll_config(struct pll_descr *descr)
{
	struct pll_config config;

	config.rate = descr->f_out;

	config.charge_pump = 2;

	config.count_m = to_counter_reg(descr->m);
	config.count_n = to_counter_reg(descr->n);
	config.count_c = to_counter_reg(descr->c);

	if(descr->f_out > 100000000)
		config.bandwith = 0x6;
	else if(descr->f_out > 130000000)
		config.bandwith = 0x5;
	else if(descr->f_out > 150000000)
		config.bandwith = 0x4;
	else if(descr->f_out > 190000000)
		config.bandwith = 0x3;
	else
		config.bandwith = 0x8;
	
	return config;
}

static void print_config(struct altera_pll *pll, struct pll_config *config)
{
	dev_dbg(pll->dev, "m           = 0x%x\n", config->count_m);
	dev_dbg(pll->dev, "n           = 0x%x\n", config->count_n);
	dev_dbg(pll->dev, "c           = 0x%x\n", config->count_c);
	dev_dbg(pll->dev, "bandwidth   = 0x%x\n", config->bandwith);
	dev_dbg(pll->dev, "charge pump = 0x%x\n", config->charge_pump);
}

// Keep the counter values as small as possible to get a stable clock
// that locks fast.
// Also take the config that is closer to the target frequency.
struct pll_descr choose_better(struct pll_descr *a, struct pll_descr *b)
{
	u32 a_sum = a->m + a->n + a->c;
	u32 b_sum = b->m + b->n + b->c;
	unsigned long a_f = a->f_out - (a->f_in / a->n * a->m / a->c);
	unsigned long b_f = b->f_out - (b->f_in / b->n * b->m / b->c);

	if(a_f || b_f) {
		if(abs(a_f) > abs(b_f))
			return *b;
		else
			return *a;
	}

	if(a_sum > b_sum)
		return *b;
	else
		return *a;
}

// Search for the next matching config.
int next(struct pll_descr *config)
{
	u32 m = config->m;
	u32 n = config->n;
	u32 c = config->c + 1;
	unsigned long f_fb;
	unsigned long c_f_out;
	unsigned long f_vco;

	for(; c < 256; ++c) {
		c_f_out = c * config->f_out;
		m = c_f_out / config->f_in;

		for(; m < 256; ++m) {
			f_fb = config->f_in * m;

			if(f_fb >= 2 * c_f_out) {
				n = f_fb / c_f_out;

				if(n == 0)
					continue;

				f_vco = f_fb / n;

				// f_vco has to be between 300 and 800 MHz.
				if((f_vco < 300000000) || (f_vco > 800000000)) {
					continue;
				}

				goto end;
			}
		}
	}

	return 0;

end:
	config->n = n;
	config->m = m;
	config->c = c;
	return 1;
}

static long get_actual_rate(struct pll_descr *descr)
{
	long f_ref = descr->f_in / descr->n;
	long f_vco = f_ref * descr->m;
	long f_out = f_vco / descr->c;

	return f_out;
}

static void print_descr(struct altera_pll *pll, struct pll_descr *descr)
{
	unsigned long f_ref = descr->f_in / descr->n;
	unsigned long f_vco = f_ref * descr->m;
	unsigned long f_out = f_vco / descr->c;

	dev_dbg(pll->dev, "m = %u\n", descr->m);
	dev_dbg(pll->dev, "n = %u\n", descr->n);
	dev_dbg(pll->dev, "c = %u\n", descr->c);
	dev_dbg(pll->dev, "f_ref = %lu\n", f_ref);
	dev_dbg(pll->dev, "f_vco = %lu\n", f_vco);
	dev_dbg(pll->dev, "f_out = %lu\n", f_out);
}

static void write_reg32(struct altera_pll *pll, int idx, u32 val)
{
  iowrite32(val, pll->mmio + (idx * 4));
}

static void write_config(struct altera_pll *pll, const struct pll_config *config)
{
  dev_dbg(pll->dev, "Setting up PLL to %lu Hz\n", config->rate);

  write_reg32(pll, ALTERA_PLL_REGIDX_MODE, 0x0); // Set waitrequest mode

  write_reg32(pll, ALTERA_PLL_REGIDX_COUNT_M,     config->count_m);
  write_reg32(pll, ALTERA_PLL_REGIDX_COUNT_N,     config->count_n);
  write_reg32(pll, ALTERA_PLL_REGIDX_COUNT_C,     config->count_c);
  write_reg32(pll, ALTERA_PLL_REGIDX_BANDWITH,    config->bandwith);
  write_reg32(pll, ALTERA_PLL_REGIDX_CHARGE_PUMP, config->charge_pump);


  write_reg32(pll, ALTERA_PLL_REGIDX_START, 0x1); // Start reconfiguration
}

/*********************/
/* clk_ops functions */
/*********************/

unsigned long altera_pll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
  struct altera_pll *pll = to_altera_pll(hw);
  dev_dbg(pll->dev, "%s = %lu\n", __func__, pll->rate);

  /* todo: In case of a reconfigurable PLL, we should get this from the hardware */

  return pll->rate;
}

long altera_pll_round_rate(struct clk_hw *hw, unsigned long rate, unsigned long *parent_rate)
{
	struct altera_pll *pll = to_altera_pll(hw);
	struct pll_descr descr;
	struct pll_descr tmp;
	
	dev_dbg(pll->dev, "%s(%lu)\n", __func__, rate);
	
	/* Return initial rate if reconfiguration is not available */
	if(pll->mmio == NULL)
	{
		if(rate == pll->rate)
			return rate;
		else
			return -1;
	}
	
	descr.f_in = 50000000;
	descr.f_out = (u32) rate;
	descr.m = 2;
	descr.n = 2;
	descr.c = 1;

	tmp = descr;
	while(next(&tmp)) {
		descr = choose_better(&descr, &tmp);
	}
	print_descr(pll, &descr);
	
	return get_actual_rate(&descr);
}

int altera_pll_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	struct altera_pll *pll = to_altera_pll(hw);
	struct pll_descr descr;
	struct pll_descr tmp;
	struct pll_config config;
	
	dev_dbg(pll->dev, "%s(%lu)\n", __func__, rate);
	
	/* Skip if reconfiguration is not available */
	if(pll->mmio == NULL)
	{
		return -1;
	}
	
	descr.f_in = 50000000;
	descr.f_out = (u32) rate;
	descr.m = 2;
	descr.n = 2;
	descr.c = 1;
	
	tmp = descr;
	while(next(&tmp)) {
		descr = choose_better(&descr, &tmp);
	}
	print_descr(pll, &descr);
	
	config = to_pll_config(&descr);

	dev_dbg(pll->dev, "Set up config:\n");
	print_config(pll, &config);

	write_config(pll, &config);
	pll->rate = config.rate;
	
	return 0;
}

int altera_pll_enable(struct clk_hw *hw)
{
  struct altera_pll *pll = to_altera_pll(hw);
  dev_dbg(pll->dev, "%s\n", __func__);

  return 0;
}

void altera_pll_disable(struct clk_hw *hw)
{
  struct altera_pll *pll = to_altera_pll(hw);
  dev_dbg(pll->dev, "%s\n", __func__);
}

static struct clk_ops altera_pll_reconf_ops = {
    .enable = altera_pll_enable,
    .disable = altera_pll_disable,
    .set_rate = altera_pll_set_rate,
    .recalc_rate = altera_pll_recalc_rate,
    .round_rate = altera_pll_round_rate,
};

static int altera_pll_probe(struct platform_device *pdev)
{
	struct altera_pll *pll;
	struct resource *mem;
	struct device_node *np = pdev->dev.of_node;
	struct clk *clk;
	struct clk_init_data init;
	
	pll = devm_kzalloc(&pdev->dev, sizeof(*pll), GFP_KERNEL);
	if(!pll)
		return -ENOMEM;
	
	pll->dev = &pdev->dev;
	
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pll->mmio = devm_ioremap_resource(pll->dev, mem);
	if(IS_ERR(pll->mmio))
	{
		dev_err(pll->dev, "Failed to map resource\n");
		return PTR_ERR(pll->mmio);
	}
	
	platform_set_drvdata(pdev, pll);
	
	init.name  = "cdc_pixel";
	init.ops = &altera_pll_reconf_ops;
	init.flags = 0;
	init.num_parents = 0;
	init.parent_names = NULL;
	pll->hw.init = &init;
	
	dev_info(pll->dev, "Registering clock %s...\n", init.name);
	clk = devm_clk_register(pll->dev, &pll->hw);
	if(IS_ERR(clk)) {
		dev_err(pll->dev, "Registering clock failed!\n");
		return PTR_ERR(clk);
	}
	
	return of_clk_add_hw_provider(np, of_clk_hw_simple_get, &pll->hw);
}

static const struct platform_device_id altera_pll_id_table[] = {
	{ "altera_pll", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, altera_pll_id_table);

static const struct of_device_id altera_pll_of_table[] = {
	{ .compatible = "altr,pll-18.0", .data = NULL },
	{ .compatible = "altr,pll", .data = NULL },
	{ }
};
MODULE_DEVICE_TABLE(of, altera_pll_of_table);

static struct platform_driver altera_pll_driver = {
	.probe = altera_pll_probe,
	.driver         = {
	        .name   = "altera_pll",
	        .of_match_table = altera_pll_of_table,
	},
	.id_table = altera_pll_id_table,
};
module_platform_driver(altera_pll_driver);

MODULE_AUTHOR("Christian Thaler <bummberumm@gmail.com>");
MODULE_DESCRIPTION("Intel PLL reconfig driver");
MODULE_LICENSE("GPL");
