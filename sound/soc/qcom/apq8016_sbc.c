/*
 * Copyright (c) 2015 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <dt-bindings/sound/apq8016-lpass.h>

struct apq8016_sbc_data {
	void __iomem *mic_iomux;
	void __iomem *spkr_iomux;
	struct snd_soc_dai_link dai_link[];	/* dynamically allocated */
};

#define MIC_CTRL_QUA_WS_SLAVE_SEL_10	BIT(17)
#define MIC_CTRL_TLMM_SCLK_EN		BIT(1)
#define	SPKR_CTL_PRI_WS_SLAVE_SEL_11	(BIT(17) | BIT(16))

static int apq8016_sbc_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct apq8016_sbc_data *pdata = snd_soc_card_get_drvdata(card);
	int rval = 0;

	switch (cpu_dai->id) {
	case MI2S_PRIMARY:
		writel(readl(pdata->spkr_iomux) | SPKR_CTL_PRI_WS_SLAVE_SEL_11,
			pdata->spkr_iomux);
		break;

	case MI2S_QUATERNARY:
		/* Configure the Quat MI2S to TLMM */
		writel(readl(pdata->mic_iomux) | MIC_CTRL_QUA_WS_SLAVE_SEL_10 |
			MIC_CTRL_TLMM_SCLK_EN,
			pdata->mic_iomux);
		break;

	default:
		dev_err(card->dev, "unsupported cpu dai configuration\n");
		rval = -EINVAL;
		break;

	}

	return rval;
}

static struct apq8016_sbc_data *apq8016_sbc_parse_of(struct snd_soc_card *card)
{
	struct device *dev = card->dev;
	struct snd_soc_dai_link *link;
	struct device_node *np, *codec, *cpu, *node  = dev->of_node;
	struct apq8016_sbc_data *data;
	int ret, num_links;

	ret = snd_soc_of_parse_card_name(card, "qcom,model");
	if (ret) {
		dev_err(dev, "Error parsing card name: %d\n", ret);
		return ERR_PTR(ret);
	}

	/* Populate links */
	num_links = of_get_child_count(node);

	/* Allocate the private data and the DAI link array */
	data = devm_kzalloc(dev, sizeof(*data) + sizeof(*link) * num_links,
			    GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	card->dai_link	= &data->dai_link[0];
	card->num_links	= num_links;

	link = data->dai_link;

	for_each_child_of_node(node, np) {
		cpu = of_get_child_by_name(np, "cpu");
		codec = of_get_child_by_name(np, "codec");

		if (!cpu || !codec) {
			dev_err(dev, "Can't find cpu/codec DT node\n");
			ret = -EINVAL;
			goto error;
		}

		link->cpu_of_node = of_parse_phandle(cpu, "sound-dai", 0);
		if (!link->cpu_of_node) {
			dev_err(card->dev, "error getting cpu phandle\n");
			ret = -EINVAL;
			goto error;
		}

		link->codec_of_node = of_parse_phandle(codec, "sound-dai", 0);
		if (!link->codec_of_node) {
			dev_err(card->dev, "error getting codec phandle\n");
			return ERR_PTR(-EINVAL);
		}

		ret = snd_soc_of_get_dai_name(cpu, &link->cpu_dai_name);
		if (ret) {
			dev_err(card->dev, "error getting cpu dai name\n");
			goto error;
		}

		ret = snd_soc_of_get_dai_name(codec, &link->codec_dai_name);
		if (ret) {
			dev_err(card->dev, "error getting codec dai name\n");
			goto error;
		}

		link->platform_of_node = link->cpu_of_node;
		/* For now we only support playback */
		link->playback_only = true;

		ret = of_property_read_string(np, "link-name", &link->name);
		if (ret) {
			dev_err(card->dev, "error getting codec dai_link name\n");
			goto error;
		}

		link->stream_name = link->name;
		link->init = apq8016_sbc_dai_init;
		link++;

		of_node_put(cpu);
		of_node_put(codec);
	}

	return data;

 error:
	of_node_put(np);
	of_node_put(cpu);
	of_node_put(codec);
	return ERR_PTR(ret);
}

static int apq8016_sbc_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card;
	struct apq8016_sbc_data *data;
	struct resource *res;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->dev = dev;
	data = apq8016_sbc_parse_of(card);
	if (IS_ERR(data)) {
		dev_err(&pdev->dev, "Error resolving dai links: %ld\n",
			PTR_ERR(data));
		return PTR_ERR(data);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mic-iomux");
	data->mic_iomux = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->mic_iomux))
		return PTR_ERR(data->mic_iomux);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "spkr-iomux");
	data->spkr_iomux = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->spkr_iomux))
		return PTR_ERR(data->spkr_iomux);

	platform_set_drvdata(pdev, data);
	snd_soc_card_set_drvdata(card, data);

	return devm_snd_soc_register_card(&pdev->dev, card);
}

static const struct of_device_id apq8016_sbc_device_id[]  = {
	{ .compatible = "qcom,apq8016-sbc-sndcard" },
	{},
};
MODULE_DEVICE_TABLE(of, apq8016_sbc_device_id);

static struct platform_driver apq8016_sbc_platform_driver = {
	.driver = {
		.name = "qcom-apq8016-sbc",
		.of_match_table = of_match_ptr(apq8016_sbc_device_id),
	},
	.probe = apq8016_sbc_platform_probe,
};
module_platform_driver(apq8016_sbc_platform_driver);

MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("APQ8016 ASoC Machine Driver");
MODULE_LICENSE("GPL v2");
