// SPDX-License-Identifier: GPL-2.0
/*
 * ALSA SoC using the QUICC Multichannel Controller (QMC)
 *
 * Copyright 2022 CS GROUP France
 *
 * Author: Herve Codina <herve.codina@bootlin.com>
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <soc/fsl/qe/qmc.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

struct qmc_dai_chan {
	struct qmc_dai_prtd *prtd_tx;
	struct qmc_dai_prtd *prtd_rx;
	struct qmc_chan *qmc_chan;
};

struct qmc_dai {
	char *name;
	int id;
	struct device *dev;
	unsigned int nb_tx_ts;
	unsigned int nb_rx_ts;

	unsigned int nb_chans_avail;
	unsigned int nb_chans_used_tx;
	unsigned int nb_chans_used_rx;
	struct qmc_dai_chan *chans;
};

struct qmc_audio {
	struct device *dev;
	unsigned int num_dais;
	struct qmc_dai *dais;
	struct snd_soc_dai_driver *dai_drivers;
};

struct qmc_dai_prtd {
	struct qmc_dai *qmc_dai;

	snd_pcm_uframes_t buffer_ended;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;

	dma_addr_t ch_dma_addr_start;
	dma_addr_t ch_dma_addr_current;
	dma_addr_t ch_dma_addr_end;
	size_t ch_dma_size;
	size_t ch_dma_offset;

	unsigned int channels;
	DECLARE_BITMAP(chans_pending, 64);
	struct snd_pcm_substream *substream;
};

static int qmc_audio_pcm_construct(struct snd_soc_component *component,
				   struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	int ret;

	ret = dma_coerce_mask_and_coherent(card->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV, card->dev,
				       64 * 1024, 64 * 1024);
	return 0;
}

static bool qmc_audio_access_is_interleaved(snd_pcm_access_t access)
{
	switch (access) {
	case SNDRV_PCM_ACCESS_MMAP_INTERLEAVED:
	case SNDRV_PCM_ACCESS_RW_INTERLEAVED:
		return true;
	default:
		break;
	}
	return false;
}

static int qmc_audio_pcm_hw_params(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct qmc_dai_prtd *prtd = substream->runtime->private_data;

	/*
	 * In interleaved mode, the driver uses one QMC channel for all audio
	 * channels whereas in non-interleaved mode, it uses one QMC channel per
	 * audio channel.
	 */
	prtd->channels = qmc_audio_access_is_interleaved(params_access(params)) ?
				1 : params_channels(params);

	prtd->substream = substream;

	prtd->buffer_ended = 0;
	prtd->buffer_size = params_buffer_size(params);
	prtd->period_size = params_period_size(params);

	prtd->ch_dma_addr_start = runtime->dma_addr;
	prtd->ch_dma_offset = params_buffer_bytes(params) / prtd->channels;
	prtd->ch_dma_addr_end = runtime->dma_addr + prtd->ch_dma_offset;
	prtd->ch_dma_addr_current = prtd->ch_dma_addr_start;
	prtd->ch_dma_size = params_period_bytes(params) / prtd->channels;

	return 0;
}

static void qmc_audio_pcm_write_complete(void *context);

static int qmc_audio_pcm_write_submit(struct qmc_dai_prtd *prtd)
{
	unsigned int i;
	int ret;

	for (i = 0; i < prtd->channels; i++) {
		bitmap_set(prtd->chans_pending, i, 1);

		ret = qmc_chan_write_submit(prtd->qmc_dai->chans[i].qmc_chan,
					    prtd->ch_dma_addr_current + i * prtd->ch_dma_offset,
					    prtd->ch_dma_size,
					    qmc_audio_pcm_write_complete,
					    &prtd->qmc_dai->chans[i]);
		if (ret) {
			dev_err(prtd->qmc_dai->dev, "write_submit %u failed %d\n",
				i, ret);
			bitmap_clear(prtd->chans_pending, i, 1);
			return ret;
		}
	}

	return 0;
}

static void qmc_audio_pcm_write_complete(void *context)
{
	struct qmc_dai_chan *chan = context;
	struct qmc_dai_prtd *prtd;

	prtd = chan->prtd_tx;

	/* Mark the current channel as completed */
	bitmap_clear(prtd->chans_pending, chan - prtd->qmc_dai->chans, 1);

	/*
	 * All QMC channels involved must have completed their transfer before
	 * submitting a new one.
	 */
	if (!bitmap_empty(prtd->chans_pending, 64))
		return;

	prtd->buffer_ended += prtd->period_size;
	if (prtd->buffer_ended >= prtd->buffer_size)
		prtd->buffer_ended = 0;

	prtd->ch_dma_addr_current += prtd->ch_dma_size;
	if (prtd->ch_dma_addr_current >= prtd->ch_dma_addr_end)
		prtd->ch_dma_addr_current = prtd->ch_dma_addr_start;

	qmc_audio_pcm_write_submit(prtd);

	snd_pcm_period_elapsed(prtd->substream);
}

static void qmc_audio_pcm_read_complete(void *context, size_t length, unsigned int flags);

static int qmc_audio_pcm_read_submit(struct qmc_dai_prtd *prtd)
{
	unsigned int i;
	int ret;

	for (i = 0; i < prtd->channels; i++) {
		bitmap_set(prtd->chans_pending, i, 1);

		ret = qmc_chan_read_submit(prtd->qmc_dai->chans[i].qmc_chan,
					   prtd->ch_dma_addr_current + i * prtd->ch_dma_offset,
					   prtd->ch_dma_size,
					   qmc_audio_pcm_read_complete,
					   &prtd->qmc_dai->chans[i]);
		if (ret) {
			dev_err(prtd->qmc_dai->dev, "read_submit %u failed %d\n",
				i, ret);
			bitmap_clear(prtd->chans_pending, i, 1);
			return ret;
		}
	}

	return 0;
}

static void qmc_audio_pcm_read_complete(void *context, size_t length, unsigned int flags)
{
	struct qmc_dai_chan *chan = context;
	struct qmc_dai_prtd *prtd;

	prtd = chan->prtd_rx;

	/* Mark the current channel as completed */
	bitmap_clear(prtd->chans_pending, chan - prtd->qmc_dai->chans, 1);

	if (length != prtd->ch_dma_size) {
		dev_err(prtd->qmc_dai->dev, "read complete length = %zu, exp %zu\n",
			length, prtd->ch_dma_size);
	}

	/*
	 * All QMC channels involved must have completed their transfer before
	 * submitting a new one.
	 */
	if (!bitmap_empty(prtd->chans_pending, 64))
		return;

	prtd->buffer_ended += prtd->period_size;
	if (prtd->buffer_ended >= prtd->buffer_size)
		prtd->buffer_ended = 0;

	prtd->ch_dma_addr_current += prtd->ch_dma_size;
	if (prtd->ch_dma_addr_current >= prtd->ch_dma_addr_end)
		prtd->ch_dma_addr_current = prtd->ch_dma_addr_start;

	qmc_audio_pcm_read_submit(prtd);

	snd_pcm_period_elapsed(prtd->substream);
}

static int qmc_audio_pcm_trigger(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream, int cmd)
{
	struct qmc_dai_prtd *prtd = substream->runtime->private_data;
	unsigned int i;
	int ret;

	if (!prtd->qmc_dai) {
		dev_err(component->dev, "qmc_dai is not set\n");
		return -EINVAL;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		bitmap_zero(prtd->chans_pending, 64);
		prtd->buffer_ended = 0;
		prtd->ch_dma_addr_current = prtd->ch_dma_addr_start;

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			for (i = 0; i < prtd->channels; i++)
				prtd->qmc_dai->chans[i].prtd_tx = prtd;

			/* Submit first chunk ... */
			ret = qmc_audio_pcm_write_submit(prtd);
			if (ret)
				return ret;

			/* ... prepare next one ... */
			prtd->ch_dma_addr_current += prtd->ch_dma_size;
			if (prtd->ch_dma_addr_current >= prtd->ch_dma_addr_end)
				prtd->ch_dma_addr_current = prtd->ch_dma_addr_start;

			/* ... and send it */
			ret = qmc_audio_pcm_write_submit(prtd);
			if (ret)
				return ret;
		} else {
			for (i = 0; i < prtd->channels; i++)
				prtd->qmc_dai->chans[i].prtd_rx = prtd;

			/* Submit first chunk ... */
			ret = qmc_audio_pcm_read_submit(prtd);
			if (ret)
				return ret;

			/* ... prepare next one ... */
			prtd->ch_dma_addr_current += prtd->ch_dma_size;
			if (prtd->ch_dma_addr_current >= prtd->ch_dma_addr_end)
				prtd->ch_dma_addr_current = prtd->ch_dma_addr_start;

			/* ... and send it */
			ret = qmc_audio_pcm_read_submit(prtd);
			if (ret)
				return ret;
		}
		break;

	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t qmc_audio_pcm_pointer(struct snd_soc_component *component,
					       struct snd_pcm_substream *substream)
{
	struct qmc_dai_prtd *prtd = substream->runtime->private_data;

	return prtd->buffer_ended;
}

static int qmc_audio_of_xlate_dai_name(struct snd_soc_component *component,
				       const struct of_phandle_args *args,
				       const char **dai_name)
{
	struct qmc_audio *qmc_audio = dev_get_drvdata(component->dev);
	struct snd_soc_dai_driver *dai_driver;
	int id = args->args[0];
	int i;

	for (i = 0; i  < qmc_audio->num_dais; i++) {
		dai_driver = qmc_audio->dai_drivers + i;
		if (dai_driver->id == id) {
			*dai_name = dai_driver->name;
			return 0;
		}
	}

	return -EINVAL;
}

static const struct snd_pcm_hardware qmc_audio_pcm_hardware = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_NONINTERLEAVED |
				  SNDRV_PCM_INFO_PAUSE,
	.period_bytes_min	= 32,
	.period_bytes_max	= 64 * 1024,
	.periods_min		= 2,
	.periods_max		= 2 * 1024,
	.buffer_bytes_max	= 64 * 1024,
};

static int qmc_audio_pcm_open(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct qmc_dai_prtd *prtd;
	int ret;

	snd_soc_set_runtime_hwparams(substream, &qmc_audio_pcm_hardware);

	/* ensure that buffer size is a multiple of period size */
	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	prtd = kzalloc(sizeof(*prtd), GFP_KERNEL);
	if (!prtd)
		return -ENOMEM;

	runtime->private_data = prtd;

	return 0;
}

static int qmc_audio_pcm_close(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream)
{
	struct qmc_dai_prtd *prtd = substream->runtime->private_data;

	kfree(prtd);
	return 0;
}

static const struct snd_soc_component_driver qmc_audio_soc_platform = {
	.open			= qmc_audio_pcm_open,
	.close			= qmc_audio_pcm_close,
	.hw_params		= qmc_audio_pcm_hw_params,
	.trigger		= qmc_audio_pcm_trigger,
	.pointer		= qmc_audio_pcm_pointer,
	.pcm_construct		= qmc_audio_pcm_construct,
	.of_xlate_dai_name	= qmc_audio_of_xlate_dai_name,
};

static unsigned int qmc_dai_get_index(struct snd_soc_dai *dai)
{
	struct qmc_audio *qmc_audio = snd_soc_dai_get_drvdata(dai);

	return dai->driver - qmc_audio->dai_drivers;
}

static struct qmc_dai *qmc_dai_get_data(struct snd_soc_dai *dai)
{
	struct qmc_audio *qmc_audio = snd_soc_dai_get_drvdata(dai);
	unsigned int index;

	index = qmc_dai_get_index(dai);
	if (index > qmc_audio->num_dais)
		return NULL;

	return qmc_audio->dais + index;
}

/*
 * The constraints for format/channel is to match with the number of 8bit
 * time-slots available.
 */
static int qmc_dai_hw_rule_channels_by_format(struct qmc_dai *qmc_dai,
					      struct snd_pcm_hw_params *params,
					      unsigned int nb_ts)
{
	struct snd_interval *c = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	snd_pcm_format_t format = params_format(params);
	struct snd_interval ch = {0};

	switch (snd_pcm_format_physical_width(format)) {
	case 8:
		ch.max = nb_ts;
		break;
	case 16:
		ch.max = nb_ts / 2;
		break;
	case 32:
		ch.max = nb_ts / 4;
		break;
	case 64:
		ch.max = nb_ts / 8;
		break;
	default:
		dev_err(qmc_dai->dev, "format physical width %u not supported\n",
			snd_pcm_format_physical_width(format));
		return -EINVAL;
	}

	ch.min = ch.max ? 1 : 0;

	return snd_interval_refine(c, &ch);
}

static int qmc_dai_hw_rule_playback_channels_by_format(struct snd_pcm_hw_params *params,
						       struct snd_pcm_hw_rule *rule)
{
	struct qmc_dai *qmc_dai = rule->private;

	return qmc_dai_hw_rule_channels_by_format(qmc_dai, params, qmc_dai->nb_tx_ts);
}

static int qmc_dai_hw_rule_capture_channels_by_format(struct snd_pcm_hw_params *params,
						      struct snd_pcm_hw_rule *rule)
{
	struct qmc_dai *qmc_dai = rule->private;

	return qmc_dai_hw_rule_channels_by_format(qmc_dai, params, qmc_dai->nb_rx_ts);
}

static int qmc_dai_hw_rule_format_by_channels(struct qmc_dai *qmc_dai,
					      struct snd_pcm_hw_params *params,
					      unsigned int nb_ts)
{
	struct snd_mask *f_old = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	unsigned int channels = params_channels(params);
	unsigned int slot_width;
	snd_pcm_format_t format;
	struct snd_mask f_new;

	if (!channels || channels > nb_ts) {
		dev_err(qmc_dai->dev, "channels %u not supported\n",
			nb_ts);
		return -EINVAL;
	}

	slot_width = (nb_ts / channels) * 8;

	snd_mask_none(&f_new);
	pcm_for_each_format(format) {
		if (snd_mask_test_format(f_old, format)) {
			if (snd_pcm_format_physical_width(format) <= slot_width)
				snd_mask_set_format(&f_new, format);
		}
	}

	return snd_mask_refine(f_old, &f_new);
}

static int qmc_dai_hw_rule_playback_format_by_channels(struct snd_pcm_hw_params *params,
						       struct snd_pcm_hw_rule *rule)
{
	struct qmc_dai *qmc_dai = rule->private;

	return qmc_dai_hw_rule_format_by_channels(qmc_dai, params, qmc_dai->nb_tx_ts);
}

static int qmc_dai_hw_rule_capture_format_by_channels(struct snd_pcm_hw_params *params,
						      struct snd_pcm_hw_rule *rule)
{
	struct qmc_dai *qmc_dai = rule->private;

	return qmc_dai_hw_rule_format_by_channels(qmc_dai, params, qmc_dai->nb_rx_ts);
}

static int qmc_dai_constraints_interleaved(struct snd_pcm_substream *substream,
					   struct qmc_dai *qmc_dai)
{
	snd_pcm_hw_rule_func_t hw_rule_channels_by_format;
	snd_pcm_hw_rule_func_t hw_rule_format_by_channels;
	unsigned int frame_bits;
	u64 access;
	int ret;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		hw_rule_channels_by_format = qmc_dai_hw_rule_capture_channels_by_format;
		hw_rule_format_by_channels = qmc_dai_hw_rule_capture_format_by_channels;
		frame_bits = qmc_dai->nb_rx_ts * 8;
	} else {
		hw_rule_channels_by_format = qmc_dai_hw_rule_playback_channels_by_format;
		hw_rule_format_by_channels = qmc_dai_hw_rule_playback_format_by_channels;
		frame_bits = qmc_dai->nb_tx_ts * 8;
	}

	ret = snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
				  hw_rule_channels_by_format, qmc_dai,
				  SNDRV_PCM_HW_PARAM_FORMAT, -1);
	if (ret) {
		dev_err(qmc_dai->dev, "Failed to add channels rule (%d)\n", ret);
		return ret;
	}

	ret = snd_pcm_hw_rule_add(substream->runtime, 0,  SNDRV_PCM_HW_PARAM_FORMAT,
				  hw_rule_format_by_channels, qmc_dai,
				  SNDRV_PCM_HW_PARAM_CHANNELS, -1);
	if (ret) {
		dev_err(qmc_dai->dev, "Failed to add format rule (%d)\n", ret);
		return ret;
	}

	ret = snd_pcm_hw_constraint_single(substream->runtime,
					   SNDRV_PCM_HW_PARAM_FRAME_BITS,
					   frame_bits);
	if (ret < 0) {
		dev_err(qmc_dai->dev, "Failed to add frame_bits constraint (%d)\n", ret);
		return ret;
	}

	access = 1ULL << (__force int)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED |
		 1ULL << (__force int)SNDRV_PCM_ACCESS_RW_INTERLEAVED;
	ret = snd_pcm_hw_constraint_mask64(substream->runtime, SNDRV_PCM_HW_PARAM_ACCESS,
					   access);
	if (ret) {
		dev_err(qmc_dai->dev, "Failed to add hw_param_access constraint (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int qmc_dai_constraints_noninterleaved(struct snd_pcm_substream *substream,
					      struct qmc_dai *qmc_dai)
{
	unsigned int frame_bits;
	u64 access;
	int ret;

	frame_bits = (substream->stream == SNDRV_PCM_STREAM_CAPTURE) ?
			qmc_dai->nb_rx_ts * 8 : qmc_dai->nb_tx_ts * 8;
	ret = snd_pcm_hw_constraint_single(substream->runtime,
					   SNDRV_PCM_HW_PARAM_FRAME_BITS,
					   frame_bits);
	if (ret < 0) {
		dev_err(qmc_dai->dev, "Failed to add frame_bits constraint (%d)\n", ret);
		return ret;
	}

	access = 1ULL << (__force int)SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED |
		 1ULL << (__force int)SNDRV_PCM_ACCESS_RW_NONINTERLEAVED;
	ret = snd_pcm_hw_constraint_mask64(substream->runtime, SNDRV_PCM_HW_PARAM_ACCESS,
					   access);
	if (ret) {
		dev_err(qmc_dai->dev, "Failed to add hw_param_access constraint (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int qmc_dai_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct qmc_dai_prtd *prtd = substream->runtime->private_data;
	struct qmc_dai *qmc_dai;

	qmc_dai = qmc_dai_get_data(dai);
	if (!qmc_dai) {
		dev_err(dai->dev, "Invalid dai\n");
		return -EINVAL;
	}

	prtd->qmc_dai = qmc_dai;

	return qmc_dai->nb_chans_avail > 1 ?
		qmc_dai_constraints_noninterleaved(substream, qmc_dai) :
		qmc_dai_constraints_interleaved(substream, qmc_dai);
}

static int qmc_dai_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct qmc_chan_param chan_param = {0};
	unsigned int nb_chans_used;
	struct qmc_dai *qmc_dai;
	unsigned int i;
	int ret;

	qmc_dai = qmc_dai_get_data(dai);
	if (!qmc_dai) {
		dev_err(dai->dev, "Invalid dai\n");
		return -EINVAL;
	}

	/*
	 * In interleaved mode, the driver uses one QMC channel for all audio
	 * channels whereas in non-interleaved mode, it uses one QMC channel per
	 * audio channel.
	 */
	nb_chans_used = qmc_audio_access_is_interleaved(params_access(params)) ?
				1 : params_channels(params);

	if (nb_chans_used > qmc_dai->nb_chans_avail) {
		dev_err(dai->dev, "Not enough qmc_chans. Need %u, avail %u\n",
			nb_chans_used, qmc_dai->nb_chans_avail);
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		chan_param.mode = QMC_TRANSPARENT;
		chan_param.transp.max_rx_buf_size = params_period_bytes(params) / nb_chans_used;
		for (i = 0; i < nb_chans_used; i++) {
			ret = qmc_chan_set_param(qmc_dai->chans[i].qmc_chan, &chan_param);
			if (ret) {
				dev_err(dai->dev, "chans[%u], set param failed %d\n",
					i, ret);
				return ret;
			}
		}
		qmc_dai->nb_chans_used_rx = nb_chans_used;
	} else {
		qmc_dai->nb_chans_used_tx = nb_chans_used;
	}

	return 0;
}

static int qmc_dai_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	unsigned int nb_chans_used;
	struct qmc_dai *qmc_dai;
	unsigned int i;
	int direction;
	int ret = 0;
	int ret_tmp;

	qmc_dai = qmc_dai_get_data(dai);
	if (!qmc_dai) {
		dev_err(dai->dev, "Invalid dai\n");
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		direction = QMC_CHAN_WRITE;
		nb_chans_used = qmc_dai->nb_chans_used_tx;
	} else {
		direction = QMC_CHAN_READ;
		nb_chans_used = qmc_dai->nb_chans_used_rx;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		for (i = 0; i < nb_chans_used; i++) {
			ret = qmc_chan_start(qmc_dai->chans[i].qmc_chan, direction);
			if (ret)
				goto err_stop;
		}
		break;

	case SNDRV_PCM_TRIGGER_STOP:
		/* Stop and reset all QMC channels and return the first error encountered */
		for (i = 0; i < nb_chans_used; i++) {
			ret_tmp = qmc_chan_stop(qmc_dai->chans[i].qmc_chan, direction);
			if (!ret)
				ret = ret_tmp;
			if (ret_tmp)
				continue;

			ret_tmp = qmc_chan_reset(qmc_dai->chans[i].qmc_chan, direction);
			if (!ret)
				ret = ret_tmp;
		}
		if (ret)
			return ret;
		break;

	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		/* Stop all QMC channels and return the first error encountered */
		for (i = 0; i < nb_chans_used; i++) {
			ret_tmp = qmc_chan_stop(qmc_dai->chans[i].qmc_chan, direction);
			if (!ret)
				ret = ret_tmp;
		}
		if (ret)
			return ret;
		break;

	default:
		return -EINVAL;
	}

	return 0;

err_stop:
	while (i--) {
		qmc_chan_stop(qmc_dai->chans[i].qmc_chan, direction);
		qmc_chan_reset(qmc_dai->chans[i].qmc_chan, direction);
	}
	return ret;
}

static const struct snd_soc_dai_ops qmc_dai_ops = {
	.startup	= qmc_dai_startup,
	.trigger	= qmc_dai_trigger,
	.hw_params	= qmc_dai_hw_params,
};

static u64 qmc_audio_formats(u8 nb_ts, bool is_noninterleaved)
{
	unsigned int format_width;
	unsigned int chan_width;
	snd_pcm_format_t format;
	u64 formats_mask;

	if (!nb_ts)
		return 0;

	formats_mask = 0;
	chan_width = nb_ts * 8;
	pcm_for_each_format(format) {
		/*
		 * Support format other than little-endian (ie big-endian or
		 * without endianness such as 8bit formats)
		 */
		if (snd_pcm_format_little_endian(format) == 1)
			continue;

		/* Support physical width multiple of 8bit */
		format_width = snd_pcm_format_physical_width(format);
		if (format_width == 0 || format_width % 8)
			continue;

		/*
		 * And support physical width that can fit N times in the
		 * channel
		 */
		if (format_width > chan_width || chan_width % format_width)
			continue;

		/*
		 * In non interleaved mode, we can only support formats that
		 * can fit only 1 time in the channel
		 */
		if (is_noninterleaved && format_width != chan_width)
			continue;

		formats_mask |= pcm_format_to_bits(format);
	}
	return formats_mask;
}

static int qmc_audio_dai_parse(struct qmc_audio *qmc_audio, struct device_node *np,
			       struct qmc_dai *qmc_dai,
			       struct snd_soc_dai_driver *qmc_soc_dai_driver)
{
	struct qmc_chan_info info;
	unsigned long rx_fs_rate;
	unsigned long tx_fs_rate;
	unsigned int nb_tx_ts;
	unsigned int nb_rx_ts;
	unsigned int i;
	int count;
	u32 val;
	int ret;

	qmc_dai->dev = qmc_audio->dev;

	ret = of_property_read_u32(np, "reg", &val);
	if (ret) {
		dev_err(qmc_audio->dev, "%pOF: failed to read reg\n", np);
		return ret;
	}
	qmc_dai->id = val;

	qmc_dai->name = devm_kasprintf(qmc_audio->dev, GFP_KERNEL, "%s.%d",
				       np->parent->name, qmc_dai->id);
	if (!qmc_dai->name)
		return -ENOMEM;

	count = qmc_chan_count_phandles(np, "fsl,qmc-chan");
	if (count < 0)
		return dev_err_probe(qmc_audio->dev, count,
				     "dai %d get number of QMC channel failed\n", qmc_dai->id);
	if (!count)
		return dev_err_probe(qmc_audio->dev, -EINVAL,
				     "dai %d no QMC channel defined\n", qmc_dai->id);

	qmc_dai->chans = devm_kcalloc(qmc_audio->dev, count, sizeof(*qmc_dai->chans), GFP_KERNEL);
	if (!qmc_dai->chans)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		qmc_dai->chans[i].qmc_chan = devm_qmc_chan_get_byphandles_index(qmc_audio->dev, np,
										"fsl,qmc-chan", i);
		if (IS_ERR(qmc_dai->chans[i].qmc_chan)) {
			return dev_err_probe(qmc_audio->dev, PTR_ERR(qmc_dai->chans[i].qmc_chan),
					     "dai %d get QMC channel %d failed\n", qmc_dai->id, i);
		}

		ret = qmc_chan_get_info(qmc_dai->chans[i].qmc_chan, &info);
		if (ret) {
			dev_err(qmc_audio->dev, "dai %d get QMC %d channel info failed %d\n",
				qmc_dai->id, i, ret);
			return ret;
		}

		if (info.mode != QMC_TRANSPARENT) {
			dev_err(qmc_audio->dev, "dai %d QMC chan %d mode %d is not QMC_TRANSPARENT\n",
				qmc_dai->id, i, info.mode);
			return -EINVAL;
		}

		/*
		 * All channels must have the same number of Tx slots and the
		 * same numbers of Rx slots.
		 */
		if (i == 0) {
			nb_tx_ts = info.nb_tx_ts;
			nb_rx_ts = info.nb_rx_ts;
			tx_fs_rate = info.tx_fs_rate;
			rx_fs_rate = info.rx_fs_rate;
		} else {
			if (nb_tx_ts != info.nb_tx_ts) {
				dev_err(qmc_audio->dev, "dai %d QMC chan %d inconsistent number of Tx timeslots (%u instead of %u)\n",
					qmc_dai->id, i, info.nb_tx_ts, nb_tx_ts);
				return -EINVAL;
			}
			if (nb_rx_ts != info.nb_rx_ts) {
				dev_err(qmc_audio->dev, "dai %d QMC chan %d inconsistent number of Rx timeslots (%u instead of %u)\n",
					qmc_dai->id, i, info.nb_rx_ts, nb_rx_ts);
				return -EINVAL;
			}
			if (tx_fs_rate != info.tx_fs_rate) {
				dev_err(qmc_audio->dev, "dai %d QMC chan %d inconsistent Tx frame sample rate (%lu instead of %lu)\n",
					qmc_dai->id, i, info.tx_fs_rate, tx_fs_rate);
				return -EINVAL;
			}
			if (rx_fs_rate != info.rx_fs_rate) {
				dev_err(qmc_audio->dev, "dai %d QMC chan %d inconsistent Rx frame sample rate (%lu instead of %lu)\n",
					qmc_dai->id, i, info.rx_fs_rate, rx_fs_rate);
				return -EINVAL;
			}
		}
	}

	qmc_dai->nb_chans_avail = count;
	qmc_dai->nb_tx_ts = nb_tx_ts * count;
	qmc_dai->nb_rx_ts = nb_rx_ts * count;

	qmc_soc_dai_driver->id = qmc_dai->id;
	qmc_soc_dai_driver->name = qmc_dai->name;

	qmc_soc_dai_driver->playback.channels_min = 0;
	qmc_soc_dai_driver->playback.channels_max = 0;
	if (nb_tx_ts) {
		qmc_soc_dai_driver->playback.channels_min = 1;
		qmc_soc_dai_driver->playback.channels_max = count > 1 ? count : nb_tx_ts;
	}
	qmc_soc_dai_driver->playback.formats = qmc_audio_formats(nb_tx_ts,
								 count > 1);

	qmc_soc_dai_driver->capture.channels_min = 0;
	qmc_soc_dai_driver->capture.channels_max = 0;
	if (nb_rx_ts) {
		qmc_soc_dai_driver->capture.channels_min = 1;
		qmc_soc_dai_driver->capture.channels_max = count > 1 ? count : nb_rx_ts;
	}
	qmc_soc_dai_driver->capture.formats = qmc_audio_formats(nb_rx_ts,
								count > 1);

	qmc_soc_dai_driver->playback.rates = snd_pcm_rate_to_rate_bit(tx_fs_rate);
	qmc_soc_dai_driver->playback.rate_min = tx_fs_rate;
	qmc_soc_dai_driver->playback.rate_max = tx_fs_rate;
	qmc_soc_dai_driver->capture.rates = snd_pcm_rate_to_rate_bit(rx_fs_rate);
	qmc_soc_dai_driver->capture.rate_min = rx_fs_rate;
	qmc_soc_dai_driver->capture.rate_max = rx_fs_rate;

	qmc_soc_dai_driver->ops = &qmc_dai_ops;

	return 0;
}

static int qmc_audio_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct qmc_audio *qmc_audio;
	struct device_node *child;
	unsigned int i;
	int ret;

	qmc_audio = devm_kzalloc(&pdev->dev, sizeof(*qmc_audio), GFP_KERNEL);
	if (!qmc_audio)
		return -ENOMEM;

	qmc_audio->dev = &pdev->dev;

	qmc_audio->num_dais = of_get_available_child_count(np);
	if (qmc_audio->num_dais) {
		qmc_audio->dais = devm_kcalloc(&pdev->dev, qmc_audio->num_dais,
					       sizeof(*qmc_audio->dais),
					       GFP_KERNEL);
		if (!qmc_audio->dais)
			return -ENOMEM;

		qmc_audio->dai_drivers = devm_kcalloc(&pdev->dev, qmc_audio->num_dais,
						      sizeof(*qmc_audio->dai_drivers),
						      GFP_KERNEL);
		if (!qmc_audio->dai_drivers)
			return -ENOMEM;
	}

	i = 0;
	for_each_available_child_of_node(np, child) {
		ret = qmc_audio_dai_parse(qmc_audio, child,
					  qmc_audio->dais + i,
					  qmc_audio->dai_drivers + i);
		if (ret) {
			of_node_put(child);
			return ret;
		}
		i++;
	}

	platform_set_drvdata(pdev, qmc_audio);

	ret = devm_snd_soc_register_component(qmc_audio->dev,
					      &qmc_audio_soc_platform,
					      qmc_audio->dai_drivers,
					      qmc_audio->num_dais);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id qmc_audio_id_table[] = {
	{ .compatible = "fsl,qmc-audio" },
	{} /* sentinel */
};
MODULE_DEVICE_TABLE(of, qmc_audio_id_table);

static struct platform_driver qmc_audio_driver = {
	.driver = {
		.name = "fsl-qmc-audio",
		.of_match_table = of_match_ptr(qmc_audio_id_table),
	},
	.probe = qmc_audio_probe,
};
module_platform_driver(qmc_audio_driver);

MODULE_AUTHOR("Herve Codina <herve.codina@bootlin.com>");
MODULE_DESCRIPTION("CPM/QE QMC audio driver");
MODULE_LICENSE("GPL");
