// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <soc/tegra/bpmp.h>
#include "mc.h"

struct tegra186_emc_dvfs {
	unsigned long latency;
	unsigned long rate;
};

struct tegra186_emc {
	struct tegra_bpmp *bpmp;
	struct device *dev;
	struct clk *clk;

	struct tegra186_emc_dvfs *dvfs;
	unsigned int num_dvfs;

	struct {
		struct dentry *root;
		unsigned long min_rate;
		unsigned long max_rate;
	} debugfs;

	struct icc_provider provider;
};

static inline struct tegra186_emc *to_tegra186_emc(struct icc_provider *provider)
{
	return container_of(provider, struct tegra186_emc, provider);
}

/*
 * debugfs interface
 *
 * The memory controller driver exposes some files in debugfs that can be used
 * to control the EMC frequency. The top-level directory can be found here:
 *
 *   /sys/kernel/debug/emc
 *
 * It contains the following files:
 *
 *   - available_rates: This file contains a list of valid, space-separated
 *     EMC frequencies.
 *
 *   - min_rate: Writing a value to this file sets the given frequency as the
 *       floor of the permitted range. If this is higher than the currently
 *       configured EMC frequency, this will cause the frequency to be
 *       increased so that it stays within the valid range.
 *
 *   - max_rate: Similarily to the min_rate file, writing a value to this file
 *       sets the given frequency as the ceiling of the permitted range. If
 *       the value is lower than the currently configured EMC frequency, this
 *       will cause the frequency to be decreased so that it stays within the
 *       valid range.
 */

static bool tegra186_emc_validate_rate(struct tegra186_emc *emc,
				       unsigned long rate)
{
	unsigned int i;

	for (i = 0; i < emc->num_dvfs; i++)
		if (rate == emc->dvfs[i].rate)
			return true;

	return false;
}

static int tegra186_emc_debug_available_rates_show(struct seq_file *s,
						   void *data)
{
	struct tegra186_emc *emc = s->private;
	const char *prefix = "";
	unsigned int i;

	for (i = 0; i < emc->num_dvfs; i++) {
		seq_printf(s, "%s%lu", prefix, emc->dvfs[i].rate);
		prefix = " ";
	}

	seq_puts(s, "\n");

	return 0;
}

static int tegra186_emc_debug_available_rates_open(struct inode *inode,
						   struct file *file)
{
	return single_open(file, tegra186_emc_debug_available_rates_show,
			   inode->i_private);
}

static const struct file_operations tegra186_emc_debug_available_rates_fops = {
	.open = tegra186_emc_debug_available_rates_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tegra186_emc_debug_min_rate_get(void *data, u64 *rate)
{
	struct tegra186_emc *emc = data;

	*rate = emc->debugfs.min_rate;

	return 0;
}

static int tegra186_emc_debug_min_rate_set(void *data, u64 rate)
{
	struct tegra186_emc *emc = data;
	int err;

	if (!tegra186_emc_validate_rate(emc, rate))
		return -EINVAL;

	err = clk_set_min_rate(emc->clk, rate);
	if (err < 0)
		return err;

	emc->debugfs.min_rate = rate;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(tegra186_emc_debug_min_rate_fops,
			  tegra186_emc_debug_min_rate_get,
			  tegra186_emc_debug_min_rate_set, "%llu\n");

static int tegra186_emc_debug_max_rate_get(void *data, u64 *rate)
{
	struct tegra186_emc *emc = data;

	*rate = emc->debugfs.max_rate;

	return 0;
}

static int tegra186_emc_debug_max_rate_set(void *data, u64 rate)
{
	struct tegra186_emc *emc = data;
	int err;

	if (!tegra186_emc_validate_rate(emc, rate))
		return -EINVAL;

	err = clk_set_max_rate(emc->clk, rate);
	if (err < 0)
		return err;

	emc->debugfs.max_rate = rate;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(tegra186_emc_debug_max_rate_fops,
			  tegra186_emc_debug_max_rate_get,
			  tegra186_emc_debug_max_rate_set, "%llu\n");

/*
 * tegra_emc_icc_set_bw() - Set BW api for EMC provider
 * @src: ICC node for External Memory Controller (EMC)
 * @dst: ICC node for External Memory (DRAM)
 *
 * Do nothing here as info to BPMP-FW is now passed in the BW set function
 * of the MC driver. BPMP-FW sets the final Freq based on the passed values.
 */
static int tegra_emc_icc_set_bw(struct icc_node *src, struct icc_node *dst)
{
	return 0;
}

static struct icc_node *
tegra_emc_of_icc_xlate(struct of_phandle_args *spec, void *data)
{
	struct icc_provider *provider = data;
	struct icc_node *node;

	/* External Memory is the only possible ICC route */
	list_for_each_entry(node, &provider->nodes, node_list) {
		if (node->id != TEGRA_ICC_EMEM)
			continue;

		return node;
	}

	return ERR_PTR(-EPROBE_DEFER);
}

static int tegra_emc_icc_get_init_bw(struct icc_node *node, u32 *avg, u32 *peak)
{
	*avg = 0;
	*peak = 0;

	return 0;
}

static int tegra_emc_interconnect_init(struct tegra186_emc *emc)
{
	struct tegra_mc *mc = dev_get_drvdata(emc->dev->parent);
	const struct tegra_mc_soc *soc = mc->soc;
	struct icc_node *node;
	int err;

	emc->provider.dev = emc->dev;
	emc->provider.set = tegra_emc_icc_set_bw;
	emc->provider.data = &emc->provider;
	emc->provider.aggregate = soc->icc_ops->aggregate;
	emc->provider.xlate = tegra_emc_of_icc_xlate;
	emc->provider.get_bw = tegra_emc_icc_get_init_bw;

	err = icc_provider_add(&emc->provider);
	if (err)
		goto err_msg;

	/* create External Memory Controller node */
	node = icc_node_create(TEGRA_ICC_EMC);
	if (IS_ERR(node)) {
		err = PTR_ERR(node);
		goto del_provider;
	}

	node->name = "External Memory Controller";
	icc_node_add(node, &emc->provider);

	/* link External Memory Controller to External Memory (DRAM) */
	err = icc_link_create(node, TEGRA_ICC_EMEM);
	if (err)
		goto remove_nodes;

	/* create External Memory node */
	node = icc_node_create(TEGRA_ICC_EMEM);
	if (IS_ERR(node)) {
		err = PTR_ERR(node);
		goto remove_nodes;
	}

	node->name = "External Memory (DRAM)";
	icc_node_add(node, &emc->provider);

	return 0;
remove_nodes:
	icc_nodes_remove(&emc->provider);
del_provider:
	icc_provider_del(&emc->provider);
err_msg:
	dev_err(emc->dev, "failed to initialize ICC: %d\n", err);

	return err;
}

static int tegra186_emc_probe(struct platform_device *pdev)
{
	struct mrq_emc_dvfs_latency_response response;
	struct tegra_bpmp_message msg;
	struct tegra186_emc *emc;
	struct tegra_mc *mc;
	unsigned int i;
	int err;

	emc = devm_kzalloc(&pdev->dev, sizeof(*emc), GFP_KERNEL);
	if (!emc)
		return -ENOMEM;

	platform_set_drvdata(pdev, emc);
	emc->dev = &pdev->dev;

	emc->bpmp = tegra_bpmp_get(&pdev->dev);
	if (IS_ERR(emc->bpmp))
		return dev_err_probe(&pdev->dev, PTR_ERR(emc->bpmp), "failed to get BPMP\n");

	emc->clk = devm_clk_get(&pdev->dev, "emc");
	if (IS_ERR(emc->clk)) {
		err = PTR_ERR(emc->clk);
		dev_err(&pdev->dev, "failed to get EMC clock: %d\n", err);
		goto put_bpmp;
	}

	platform_set_drvdata(pdev, emc);
	emc->dev = &pdev->dev;

	if (tegra_bpmp_mrq_is_supported(emc->bpmp, MRQ_EMC_DVFS_LATENCY)) {
		memset(&msg, 0, sizeof(msg));
		msg.mrq = MRQ_EMC_DVFS_LATENCY;
		msg.tx.data = NULL;
		msg.tx.size = 0;
		msg.rx.data = &response;
		msg.rx.size = sizeof(response);

		err = tegra_bpmp_transfer(emc->bpmp, &msg);
		if (err < 0) {
			dev_err(&pdev->dev, "failed to EMC DVFS pairs: %d\n", err);
			goto put_bpmp;
		}
		if (msg.rx.ret < 0) {
			err = -EINVAL;
			dev_err(&pdev->dev, "EMC DVFS MRQ failed: %d (BPMP error code)\n", msg.rx.ret);
			goto put_bpmp;
		}

		emc->debugfs.min_rate = ULONG_MAX;
		emc->debugfs.max_rate = 0;

		emc->num_dvfs = response.num_pairs;

		emc->dvfs = devm_kmalloc_array(&pdev->dev, emc->num_dvfs,
					       sizeof(*emc->dvfs), GFP_KERNEL);
		if (!emc->dvfs) {
			err = -ENOMEM;
			goto put_bpmp;
		}

		dev_dbg(&pdev->dev, "%u DVFS pairs:\n", emc->num_dvfs);

		for (i = 0; i < emc->num_dvfs; i++) {
			emc->dvfs[i].rate = response.pairs[i].freq * 1000;
			emc->dvfs[i].latency = response.pairs[i].latency;

			if (emc->dvfs[i].rate < emc->debugfs.min_rate)
				emc->debugfs.min_rate = emc->dvfs[i].rate;

			if (emc->dvfs[i].rate > emc->debugfs.max_rate)
				emc->debugfs.max_rate = emc->dvfs[i].rate;

			dev_dbg(&pdev->dev, "  %2u: %lu Hz -> %lu us\n", i,
				emc->dvfs[i].rate, emc->dvfs[i].latency);
		}

		err = clk_set_rate_range(emc->clk, emc->debugfs.min_rate,
					 emc->debugfs.max_rate);
		if (err < 0) {
			dev_err(&pdev->dev,
				"failed to set rate range [%lu-%lu] for %pC\n",
				emc->debugfs.min_rate, emc->debugfs.max_rate,
				emc->clk);
			goto put_bpmp;
		}

		emc->debugfs.root = debugfs_create_dir("emc", NULL);
		debugfs_create_file("available_rates", S_IRUGO, emc->debugfs.root,
				    emc, &tegra186_emc_debug_available_rates_fops);
		debugfs_create_file("min_rate", S_IRUGO | S_IWUSR, emc->debugfs.root,
				    emc, &tegra186_emc_debug_min_rate_fops);
		debugfs_create_file("max_rate", S_IRUGO | S_IWUSR, emc->debugfs.root,
				    emc, &tegra186_emc_debug_max_rate_fops);
	}

	mc = dev_get_drvdata(emc->dev->parent);
	if (mc && mc->soc->icc_ops) {
		err = tegra_emc_interconnect_init(emc);
		if (err)
			goto put_bpmp;

		if (tegra_bpmp_mrq_is_supported(emc->bpmp, MRQ_BWMGR_INT))
			mc->bwmgr_mrq_supported = true;
		else
			dev_info(&pdev->dev, "MRQ_BWMGR_INT not present\n");
	}

	return 0;

put_bpmp:
	tegra_bpmp_put(emc->bpmp);
	return err;
}

static int tegra186_emc_remove(struct platform_device *pdev)
{
	struct tegra186_emc *emc = platform_get_drvdata(pdev);

	debugfs_remove_recursive(emc->debugfs.root);
	tegra_bpmp_put(emc->bpmp);

	return 0;
}

static const struct of_device_id tegra186_emc_of_match[] = {
#if defined(CONFIG_ARCH_TEGRA_186_SOC)
	{ .compatible = "nvidia,tegra186-emc" },
#endif
#if defined(CONFIG_ARCH_TEGRA_194_SOC)
	{ .compatible = "nvidia,tegra194-emc" },
#endif
#if defined(CONFIG_ARCH_TEGRA_234_SOC)
	{ .compatible = "nvidia,tegra234-emc" },
#endif
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra186_emc_of_match);

static struct platform_driver tegra186_emc_driver = {
	.driver = {
		.name = "tegra186-emc",
		.of_match_table = tegra186_emc_of_match,
		.suppress_bind_attrs = true,
		.sync_state = icc_sync_state,
	},
	.probe = tegra186_emc_probe,
	.remove = tegra186_emc_remove,
};
module_platform_driver(tegra186_emc_driver);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra186 External Memory Controller driver");
MODULE_LICENSE("GPL v2");
