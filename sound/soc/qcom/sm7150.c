// SPDX-License-Identifier: GPL-2.0
/*
 * ASoC machine driver for SM7150 boards using the APR (q6afe/q6asm/q6adm)
 * audio DSP. Currently handles the Google Pixel 4a (sunfish) loudspeakers:
 * two Cirrus CS35L41 amplifiers on the tertiary TDM bus.
 */

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/cs35l41.h>
#include "common.h"
#include "qdsp6/q6afe.h"

#define DRIVER_NAME		"sm7150"
#define DEFAULT_SAMPLE_RATE_48K	48000
#define TDM_SLOTS		8
#define TDM_SLOT_WIDTH		16
#define TDM_BCLK_RATE		(DEFAULT_SAMPLE_RATE_48K * TDM_SLOTS * TDM_SLOT_WIDTH)

struct sm7150_snd_data {
	struct snd_soc_card *card;
	unsigned int tert_tdm_clk_count;
};

static unsigned int tdm_slot_offset[TDM_SLOTS] = {0, 4, 8, 12, 16, 20, 24, 28};

/*
 * Each amp plays ASP RX1; point it at its own TDM slot so the left amp
 * gets channel 0 and the right amp channel 1.
 */
static const unsigned int cs35l41_left_rx[] = { 0, 1 };
static const unsigned int cs35l41_right_rx[] = { 1, 0 };

static int sm7150_tdm_snd_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	const unsigned int *rx;
	int ret, i;

	ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0x3, TDM_SLOTS,
				       TDM_SLOT_WIDTH);
	if (ret < 0) {
		dev_err(rtd->dev, "failed to set cpu tdm slot: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
					  params_channels(params),
					  tdm_slot_offset);
	if (ret < 0) {
		dev_err(rtd->dev, "failed to set cpu channel map: %d\n", ret);
		return ret;
	}

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		const char *prefix = codec_dai->component->name_prefix;

		ret = snd_soc_dai_set_sysclk(codec_dai, CS35L41_CLKID_SCLK,
					     TDM_BCLK_RATE, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set sysclk: %d\n",
				prefix, ret);
			return ret;
		}

		ret = snd_soc_component_set_sysclk(codec_dai->component,
						   CS35L41_CLKID_SCLK, 0,
						   TDM_BCLK_RATE,
						   SND_SOC_CLOCK_IN);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set component sysclk: %d\n",
				prefix, ret);
			return ret;
		}

		rx = (prefix && !strcmp(prefix, "R")) ? cs35l41_right_rx
						      : cs35l41_left_rx;
		ret = snd_soc_dai_set_channel_map(codec_dai, 0, NULL, 2, rx);
		if (ret < 0) {
			dev_err(rtd->dev, "%s: failed to set channel map: %d\n",
				prefix, ret);
			return ret;
		}
	}

	return 0;
}

static int sm7150_snd_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case TERTIARY_TDM_RX_0:
		return sm7150_tdm_snd_hw_params(substream, params);
	default:
		return 0;
	}
}

static int sm7150_snd_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm7150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	int ret, i;

	switch (cpu_dai->id) {
	case TERTIARY_TDM_RX_0:
		if (++data->tert_tdm_clk_count == 1)
			snd_soc_dai_set_sysclk(cpu_dai,
					       Q6AFE_LPASS_CLK_ID_TER_TDM_IBIT,
					       TDM_BCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);

		/* Short frame sync, one bit of data delay, no inversion */
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			ret = snd_soc_dai_set_fmt(codec_dai,
						  SND_SOC_DAIFMT_CBC_CFC |
						  SND_SOC_DAIFMT_NB_NF |
						  SND_SOC_DAIFMT_DSP_A);
			if (ret < 0) {
				dev_err(rtd->dev, "failed to set codec fmt: %d\n",
					ret);
				return ret;
			}
		}
		break;
	default:
		break;
	}

	return 0;
}

static void sm7150_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm7150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case TERTIARY_TDM_RX_0:
		if (--data->tert_tdm_clk_count == 0)
			snd_soc_dai_set_sysclk(cpu_dai,
					       Q6AFE_LPASS_CLK_ID_TER_TDM_IBIT,
					       0, SNDRV_PCM_STREAM_PLAYBACK);
		break;
	default:
		break;
	}
}

static const struct snd_soc_ops sm7150_be_ops = {
	.hw_params = sm7150_snd_hw_params,
	.startup = sm7150_snd_startup,
	.shutdown = sm7150_snd_shutdown,
};

static int sm7150_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
							  SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = DEFAULT_SAMPLE_RATE_48K;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static const struct snd_soc_dapm_widget sm7150_snd_widgets[] = {
	SND_SOC_DAPM_SPK("Left Spk", NULL),
	SND_SOC_DAPM_SPK("Right Spk", NULL),
};

static void sm7150_add_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->ops = &sm7150_be_ops;
			link->be_hw_params_fixup = sm7150_be_hw_params_fixup;
		}
	}
}

static int sm7150_snd_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sm7150_snd_data *data;
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->driver_name = DRIVER_NAME;
	card->dapm_widgets = sm7150_snd_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(sm7150_snd_widgets);
	card->dev = dev;
	card->owner = THIS_MODULE;
	dev_set_drvdata(dev, card);

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	data->card = card;
	snd_soc_card_set_drvdata(card, data);

	sm7150_add_ops(card);

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id sm7150_snd_device_id[] = {
	{ .compatible = "qcom,sm7150-sndcard" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm7150_snd_device_id);

static struct platform_driver sm7150_snd_driver = {
	.probe = sm7150_snd_platform_probe,
	.driver = {
		.name = "msm-snd-sm7150",
		.of_match_table = sm7150_snd_device_id,
	},
};
module_platform_driver(sm7150_snd_driver);

MODULE_DESCRIPTION("SM7150 ASoC Machine Driver");
MODULE_LICENSE("GPL");
