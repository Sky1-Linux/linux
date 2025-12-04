// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024-2025 Arm Limited
// Arm DMA-350 driver

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include "dmaengine.h"
#include "virt-dma.h"

#define DMAINFO			0x0f00

#define DMA_BUILDCFG0		0xb0
#define DMA_CFG_DATA_WIDTH	GENMASK(18, 16)
#define DMA_CFG_ADDR_WIDTH	GENMASK(15, 10)
#define DMA_CFG_NUM_CHANNELS	GENMASK(9, 4)

#define DMA_BUILDCFG1		0xb4
#define DMA_CFG_NUM_TRIGGER_IN	GENMASK(8, 0)

#define IIDR			0xc8
#define IIDR_PRODUCTID		GENMASK(31, 20)
#define IIDR_VARIANT		GENMASK(19, 16)
#define IIDR_REVISION		GENMASK(15, 12)
#define IIDR_IMPLEMENTER	GENMASK(11, 0)

#define PRODUCTID_DMA350	0x3a0
#define IMPLEMENTER_ARM		0x43b

#define DMACH(n)		(0x1000 + 0x0100 * (n))

#define CH_CMD			0x00
#define CH_CMD_RESUME		BIT(5)
#define CH_CMD_PAUSE		BIT(4)
#define CH_CMD_STOP		BIT(3)
#define CH_CMD_DISABLE		BIT(2)
#define CH_CMD_CLEAR		BIT(1)
#define CH_CMD_ENABLE		BIT(0)

#define CH_STATUS		0x04
#define CH_STAT_RESUMEWAIT	BIT(21)
#define CH_STAT_PAUSED		BIT(20)
#define CH_STAT_STOPPED		BIT(19)
#define CH_STAT_DISABLED	BIT(18)
#define CH_STAT_ERR		BIT(17)
#define CH_STAT_DONE		BIT(16)
#define CH_STAT_INTR_ERR	BIT(1)
#define CH_STAT_INTR_DONE	BIT(0)

#define CH_INTREN		0x08
#define CH_INTREN_ERR		BIT(1)
#define CH_INTREN_DONE		BIT(0)

#define CH_CTRL			0x0c
#define CH_CTRL_USEDESTRIGIN	BIT(26)
#define CH_CTRL_USESRCTRIGIN	BIT(26)
#define CH_CTRL_DONETYPE	GENMASK(23, 21)
#define CH_CTRL_REGRELOADTYPE	GENMASK(20, 18)
#define CH_CTRL_XTYPE		GENMASK(11, 9)
#define CH_CTRL_TRANSIZE	GENMASK(2, 0)

#define CH_SRCADDR		0x10
#define CH_SRCADDRHI		0x14
#define CH_DESADDR		0x18
#define CH_DESADDRHI		0x1c
#define CH_XSIZE		0x20
#define CH_XSIZEHI		0x24
#define CH_SRCTRANSCFG		0x28
#define CH_DESTRANSCFG		0x2c
#define CH_CFG_MAXBURSTLEN	GENMASK(19, 16)
#define CH_CFG_PRIVATTR		BIT(11)
#define CH_CFG_SHAREATTR	GENMASK(9, 8)
#define CH_CFG_MEMATTR		GENMASK(7, 0)

#define TRANSCFG_DEVICE					\
	FIELD_PREP(CH_CFG_MAXBURSTLEN, 0xf) |		\
	FIELD_PREP(CH_CFG_SHAREATTR, SHAREATTR_OSH) |	\
	FIELD_PREP(CH_CFG_MEMATTR, MEMATTR_DEVICE)
#define TRANSCFG_NC					\
	FIELD_PREP(CH_CFG_MAXBURSTLEN, 0xf) |		\
	FIELD_PREP(CH_CFG_SHAREATTR, SHAREATTR_OSH) |	\
	FIELD_PREP(CH_CFG_MEMATTR, MEMATTR_NC)
#define TRANSCFG_WB					\
	FIELD_PREP(CH_CFG_MAXBURSTLEN, 0xf) |		\
	FIELD_PREP(CH_CFG_SHAREATTR, SHAREATTR_ISH) |	\
	FIELD_PREP(CH_CFG_MEMATTR, MEMATTR_WB)

#define CH_XADDRINC		0x30
#define CH_XY_DES		GENMASK(31, 16)
#define CH_XY_SRC		GENMASK(15, 0)

#define CH_FILLVAL		0x38
#define CH_SRCTRIGINCFG		0x4c
#define CH_DESTRIGINCFG		0x50
#define CH_LINKATTR		0x70
#define CH_LINK_SHAREATTR	GENMASK(9, 8)
#define CH_LINK_MEMATTR		GENMASK(7, 0)

#define CH_AUTOCFG		0x74
#define CH_LINKADDR		0x78
#define CH_LINKADDR_EN		BIT(0)

#define CH_LINKADDRHI		0x7c
#define CH_ERRINFO		0x90
#define CH_ERRINFO_AXIRDPOISERR BIT(18)
#define CH_ERRINFO_AXIWRRESPERR BIT(17)
#define CH_ERRINFO_AXIRDRESPERR BIT(16)

#define CH_BUILDCFG0		0xf8
#define CH_CFG_INC_WIDTH	GENMASK(29, 26)
#define CH_CFG_DATA_WIDTH	GENMASK(24, 22)
#define CH_CFG_DATA_BUF_SIZE	GENMASK(7, 0)

#define CH_BUILDCFG1		0xfc
#define CH_CFG_HAS_CMDLINK	BIT(8)
#define CH_CFG_HAS_TRIGSEL	BIT(7)
#define CH_CFG_HAS_TRIGIN	BIT(5)
#define CH_CFG_HAS_WRAP		BIT(1)


#define LINK_REGCLEAR		BIT(0)
#define LINK_INTREN		BIT(2)
#define LINK_CTRL		BIT(3)
#define LINK_SRCADDR		BIT(4)
#define LINK_SRCADDRHI		BIT(5)
#define LINK_DESADDR		BIT(6)
#define LINK_DESADDRHI		BIT(7)
#define LINK_XSIZE		BIT(8)
#define LINK_XSIZEHI		BIT(9)
#define LINK_SRCTRANSCFG	BIT(10)
#define LINK_DESTRANSCFG	BIT(11)
#define LINK_XADDRINC		BIT(12)
#define LINK_FILLVAL		BIT(14)
#define LINK_SRCTRIGINCFG	BIT(19)
#define LINK_DESTRIGINCFG	BIT(20)
#define LINK_AUTOCFG		BIT(29)
#define LINK_LINKADDR		BIT(30)
#define LINK_LINKADDRHI		BIT(31)


enum ch_ctrl_donetype {
	CH_CTRL_DONETYPE_NONE = 0,
	CH_CTRL_DONETYPE_CMD = 1,
	CH_CTRL_DONETYPE_CYCLE = 3
};

enum ch_ctrl_xtype {
	CH_CTRL_XTYPE_DISABLE = 0,
	CH_CTRL_XTYPE_CONTINUE = 1,
	CH_CTRL_XTYPE_WRAP = 2,
	CH_CTRL_XTYPE_FILL = 3
};

enum ch_cfg_shareattr {
	SHAREATTR_NSH = 0,
	SHAREATTR_OSH = 2,
	SHAREATTR_ISH = 3
};

enum ch_cfg_memattr {
	MEMATTR_DEVICE = 0x00,
	MEMATTR_NC = 0x44,
	MEMATTR_WB = 0xff
};

struct d350_desc {
	struct virt_dma_desc vd;
	u32 command[16];
	u16 xsize;
	u16 xsizehi;
	u8 tsz;
};

struct d350_chan {
	struct virt_dma_chan vc;
	struct d350_desc *desc;
	void __iomem *base;
	int irq;
	enum dma_status status;
	dma_cookie_t cookie;
	u32 residue;
	u8 tsz;
	bool has_trig;
	bool has_wrap;
	bool coherent;
	/* Slave DMA configuration */
	struct dma_slave_config slave_cfg;
	u32 req_line;		/* DMA request/trigger line */
	dma_addr_t dev_addr;	/* Peripheral address */
	enum dma_transfer_direction dir;
};

struct d350 {
	struct dma_device dma;
	struct clk *clk;
	struct reset_control *rst;
	int nchan;
	int nreq;
	struct d350_chan channels[] __counted_by(nchan);
};

static inline struct d350_chan *to_d350_chan(struct dma_chan *chan)
{
	return container_of(chan, struct d350_chan, vc.chan);
}

static inline struct d350_desc *to_d350_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct d350_desc, vd);
}

static void d350_desc_free(struct virt_dma_desc *vd)
{
	kfree(to_d350_desc(vd));
}

static struct dma_async_tx_descriptor *d350_prep_memcpy(struct dma_chan *chan,
		dma_addr_t dest, dma_addr_t src, size_t len, unsigned long flags)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct d350_desc *desc;
	u32 *cmd;

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->tsz = __ffs(len | dest | src | (1 << dch->tsz));
	desc->xsize = lower_16_bits(len >> desc->tsz);
	desc->xsizehi = upper_16_bits(len >> desc->tsz);

	cmd = desc->command;
	cmd[0] = LINK_CTRL | LINK_SRCADDR | LINK_SRCADDRHI | LINK_DESADDR |
		 LINK_DESADDRHI | LINK_XSIZE | LINK_XSIZEHI | LINK_SRCTRANSCFG |
		 LINK_DESTRANSCFG | LINK_XADDRINC | LINK_LINKADDR;

	cmd[1] = FIELD_PREP(CH_CTRL_TRANSIZE, desc->tsz) |
		 FIELD_PREP(CH_CTRL_XTYPE, CH_CTRL_XTYPE_CONTINUE) |
		 FIELD_PREP(CH_CTRL_DONETYPE, CH_CTRL_DONETYPE_CMD);

	cmd[2] = lower_32_bits(src);
	cmd[3] = upper_32_bits(src);
	cmd[4] = lower_32_bits(dest);
	cmd[5] = upper_32_bits(dest);
	cmd[6] = FIELD_PREP(CH_XY_SRC, desc->xsize) | FIELD_PREP(CH_XY_DES, desc->xsize);
	cmd[7] = FIELD_PREP(CH_XY_SRC, desc->xsizehi) | FIELD_PREP(CH_XY_DES, desc->xsizehi);
	cmd[8] = dch->coherent ? TRANSCFG_WB : TRANSCFG_NC;
	cmd[9] = dch->coherent ? TRANSCFG_WB : TRANSCFG_NC;
	cmd[10] = FIELD_PREP(CH_XY_SRC, 1) | FIELD_PREP(CH_XY_DES, 1);
	cmd[11] = 0;

	return vchan_tx_prep(&dch->vc, &desc->vd, flags);
}

static struct dma_async_tx_descriptor *d350_prep_memset(struct dma_chan *chan,
		dma_addr_t dest, int value, size_t len, unsigned long flags)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct d350_desc *desc;
	u32 *cmd;

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->tsz = __ffs(len | dest | (1 << dch->tsz));
	desc->xsize = lower_16_bits(len >> desc->tsz);
	desc->xsizehi = upper_16_bits(len >> desc->tsz);

	cmd = desc->command;
	cmd[0] = LINK_CTRL | LINK_DESADDR | LINK_DESADDRHI |
		 LINK_XSIZE | LINK_XSIZEHI | LINK_DESTRANSCFG |
		 LINK_XADDRINC | LINK_FILLVAL | LINK_LINKADDR;

	cmd[1] = FIELD_PREP(CH_CTRL_TRANSIZE, desc->tsz) |
		 FIELD_PREP(CH_CTRL_XTYPE, CH_CTRL_XTYPE_FILL) |
		 FIELD_PREP(CH_CTRL_DONETYPE, CH_CTRL_DONETYPE_CMD);

	cmd[2] = lower_32_bits(dest);
	cmd[3] = upper_32_bits(dest);
	cmd[4] = FIELD_PREP(CH_XY_DES, desc->xsize);
	cmd[5] = FIELD_PREP(CH_XY_DES, desc->xsizehi);
	cmd[6] = dch->coherent ? TRANSCFG_WB : TRANSCFG_NC;
	cmd[7] = FIELD_PREP(CH_XY_DES, 1);
	cmd[8] = (u8)value * 0x01010101;
	cmd[9] = 0;

	return vchan_tx_prep(&dch->vc, &desc->vd, flags);
}

static int d350_pause(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (dch->status == DMA_IN_PROGRESS) {
		writel_relaxed(CH_CMD_PAUSE, dch->base + CH_CMD);
		dch->status = DMA_PAUSED;
	}
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	return 0;
}

static int d350_resume(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (dch->status == DMA_PAUSED) {
		writel_relaxed(CH_CMD_RESUME, dch->base + CH_CMD);
		dch->status = DMA_IN_PROGRESS;
	}
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	return 0;
}

static u32 d350_get_residue(struct d350_chan *dch)
{
	u32 res, xsize, xsizehi, hi_new;
	int retries = 3; /* 1st time unlucky, 2nd improbable, 3rd just broken */

	hi_new = readl_relaxed(dch->base + CH_XSIZEHI);
	do {
		xsizehi = hi_new;
		xsize = readl_relaxed(dch->base + CH_XSIZE);
		hi_new = readl_relaxed(dch->base + CH_XSIZEHI);
	} while (xsizehi != hi_new && --retries);

	res = FIELD_GET(CH_XY_DES, xsize);
	res |= FIELD_GET(CH_XY_DES, xsizehi) << 16;

	return res << dch->desc->tsz;
}

static int d350_terminate_all(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	unsigned long flags;
	LIST_HEAD(list);

	spin_lock_irqsave(&dch->vc.lock, flags);
	writel_relaxed(CH_CMD_STOP, dch->base + CH_CMD);
	if (dch->desc) {
		if (dch->status != DMA_ERROR)
			vchan_terminate_vdesc(&dch->desc->vd);
		dch->desc = NULL;
		dch->status = DMA_COMPLETE;
	}
	vchan_get_all_descriptors(&dch->vc, &list);
	list_splice_tail(&list, &dch->vc.desc_terminated);
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	return 0;
}

static void d350_synchronize(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);

	vchan_synchronize(&dch->vc);
}

static u32 d350_desc_bytes(struct d350_desc *desc)
{
	return ((u32)desc->xsizehi << 16 | desc->xsize) << desc->tsz;
}

static enum dma_status d350_tx_status(struct dma_chan *chan, dma_cookie_t cookie,
				      struct dma_tx_state *state)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct virt_dma_desc *vd;
	enum dma_status status;
	unsigned long flags;
	u32 residue = 0;

	status = dma_cookie_status(chan, cookie, state);

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (cookie == dch->cookie) {
		status = dch->status;
		if (status == DMA_IN_PROGRESS || status == DMA_PAUSED)
			dch->residue = d350_get_residue(dch);
		residue = dch->residue;
	} else if ((vd = vchan_find_desc(&dch->vc, cookie))) {
		residue = d350_desc_bytes(to_d350_desc(vd));
	} else if (status == DMA_IN_PROGRESS) {
		/* Somebody else terminated it? */
		status = DMA_ERROR;
	}
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	dma_set_residue(state, residue);
	return status;
}

static void d350_start_next(struct d350_chan *dch)
{
	u32 hdr, *reg;

	dch->desc = to_d350_desc(vchan_next_desc(&dch->vc));
	if (!dch->desc)
		return;

	list_del(&dch->desc->vd.node);
	dch->status = DMA_IN_PROGRESS;
	dch->cookie = dch->desc->vd.tx.cookie;
	dch->residue = d350_desc_bytes(dch->desc);

	hdr = dch->desc->command[0];
	reg = &dch->desc->command[1];

	if (hdr & LINK_INTREN)
		writel_relaxed(*reg++, dch->base + CH_INTREN);
	if (hdr & LINK_CTRL)
		writel_relaxed(*reg++, dch->base + CH_CTRL);
	if (hdr & LINK_SRCADDR)
		writel_relaxed(*reg++, dch->base + CH_SRCADDR);
	if (hdr & LINK_SRCADDRHI)
		writel_relaxed(*reg++, dch->base + CH_SRCADDRHI);
	if (hdr & LINK_DESADDR)
		writel_relaxed(*reg++, dch->base + CH_DESADDR);
	if (hdr & LINK_DESADDRHI)
		writel_relaxed(*reg++, dch->base + CH_DESADDRHI);
	if (hdr & LINK_XSIZE)
		writel_relaxed(*reg++, dch->base + CH_XSIZE);
	if (hdr & LINK_XSIZEHI)
		writel_relaxed(*reg++, dch->base + CH_XSIZEHI);
	if (hdr & LINK_SRCTRANSCFG)
		writel_relaxed(*reg++, dch->base + CH_SRCTRANSCFG);
	if (hdr & LINK_DESTRANSCFG)
		writel_relaxed(*reg++, dch->base + CH_DESTRANSCFG);
	if (hdr & LINK_XADDRINC)
		writel_relaxed(*reg++, dch->base + CH_XADDRINC);
	if (hdr & LINK_FILLVAL)
		writel_relaxed(*reg++, dch->base + CH_FILLVAL);
	if (hdr & LINK_SRCTRIGINCFG)
		writel_relaxed(*reg++, dch->base + CH_SRCTRIGINCFG);
	if (hdr & LINK_DESTRIGINCFG)
		writel_relaxed(*reg++, dch->base + CH_DESTRIGINCFG);
	if (hdr & LINK_AUTOCFG)
		writel_relaxed(*reg++, dch->base + CH_AUTOCFG);
	if (hdr & LINK_LINKADDR)
		writel_relaxed(*reg++, dch->base + CH_LINKADDR);
	if (hdr & LINK_LINKADDRHI)
		writel_relaxed(*reg++, dch->base + CH_LINKADDRHI);

	writel(CH_CMD_ENABLE, dch->base + CH_CMD);
}

static void d350_issue_pending(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (vchan_issue_pending(&dch->vc) && !dch->desc)
		d350_start_next(dch);
	spin_unlock_irqrestore(&dch->vc.lock, flags);
}

static irqreturn_t d350_irq(int irq, void *data)
{
	struct d350_chan *dch = data;
	struct device *dev = dch->vc.chan.device->dev;
	struct virt_dma_desc *vd = &dch->desc->vd;
	u32 ch_status;

	ch_status = readl(dch->base + CH_STATUS);
	if (!ch_status)
		return IRQ_NONE;

	if (ch_status & CH_STAT_INTR_ERR) {
		u32 errinfo = readl_relaxed(dch->base + CH_ERRINFO);

		if (errinfo & (CH_ERRINFO_AXIRDPOISERR | CH_ERRINFO_AXIRDRESPERR))
			vd->tx_result.result = DMA_TRANS_READ_FAILED;
		else if (errinfo & CH_ERRINFO_AXIWRRESPERR)
			vd->tx_result.result = DMA_TRANS_WRITE_FAILED;
		else
			vd->tx_result.result = DMA_TRANS_ABORTED;

		vd->tx_result.residue = d350_get_residue(dch);
	} else if (!(ch_status & CH_STAT_INTR_DONE)) {
		dev_warn(dev, "Unexpected IRQ source? 0x%08x\n", ch_status);
	}
	writel_relaxed(ch_status, dch->base + CH_STATUS);

	spin_lock(&dch->vc.lock);
	vchan_cookie_complete(vd);
	if (ch_status & CH_STAT_INTR_DONE) {
		dch->status = DMA_COMPLETE;
		dch->residue = 0;
		d350_start_next(dch);
	} else {
		dch->status = DMA_ERROR;
		dch->residue = vd->tx_result.residue;
	}
	spin_unlock(&dch->vc.lock);

	return IRQ_HANDLED;
}

static int d350_alloc_chan_resources(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	int ret = request_irq(dch->irq, d350_irq, IRQF_SHARED,
			      dev_name(&dch->vc.chan.dev->device), dch);
	if (!ret)
		writel_relaxed(CH_INTREN_DONE | CH_INTREN_ERR, dch->base + CH_INTREN);

	return ret;
}

static void d350_free_chan_resources(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);

	writel_relaxed(0, dch->base + CH_INTREN);
	free_irq(dch->irq, dch);
	vchan_free_chan_resources(&dch->vc);
}

/*
 * Slave DMA support
 */
static int d350_device_config(struct dma_chan *chan,
			      struct dma_slave_config *config)
{
	struct d350_chan *dch = to_d350_chan(chan);

	memcpy(&dch->slave_cfg, config, sizeof(*config));

	if (config->direction == DMA_MEM_TO_DEV) {
		dch->dev_addr = config->dst_addr;
		dch->dir = DMA_MEM_TO_DEV;
	} else if (config->direction == DMA_DEV_TO_MEM) {
		dch->dev_addr = config->src_addr;
		dch->dir = DMA_DEV_TO_MEM;
	}

	return 0;
}

static struct dma_chan *d350_of_xlate(struct of_phandle_args *dma_spec,
				      struct of_dma *ofdma)
{
	struct d350 *dmac = ofdma->of_dma_data;
	struct dma_chan *chan;
	struct d350_chan *dch;
	u32 request = 0;
	u32 channel_id = 0;

	if (dma_spec->args_count >= 1)
		request = dma_spec->args[0];
	if (dma_spec->args_count >= 2)
		channel_id = dma_spec->args[1];

	/* Channel 0xFF means dynamic allocation */
	if (channel_id == 0xFF) {
		chan = dma_get_any_slave_channel(&dmac->dma);
	} else {
		if (channel_id >= dmac->nchan)
			return NULL;
		chan = dma_get_slave_channel(&dmac->channels[channel_id].vc.chan);
	}

	if (!chan)
		return NULL;

	dch = to_d350_chan(chan);
	dch->req_line = request;

	return chan;
}

static struct dma_async_tx_descriptor *d350_prep_slave_sg(
	struct dma_chan *chan, struct scatterlist *sgl, unsigned int sg_len,
	enum dma_transfer_direction dir, unsigned long flags, void *context)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct device *dev = chan->device->dev;

	/* For now, just log that slave_sg was called - full implementation needed */
	dev_dbg(dev, "prep_slave_sg: chan=%d dir=%d sg_len=%d\n",
		dch->vc.chan.chan_id, dir, sg_len);

	/* TODO: Implement scatter-gather slave DMA */
	return NULL;
}

static struct dma_async_tx_descriptor *d350_prep_dma_cyclic(
	struct dma_chan *chan, dma_addr_t buf_addr, size_t buf_len,
	size_t period_len, enum dma_transfer_direction dir,
	unsigned long flags)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct device *dev = chan->device->dev;

	/* For now, just log that cyclic was called - full implementation needed */
	dev_dbg(dev, "prep_dma_cyclic: chan=%d dir=%d buf_len=%zu period_len=%zu\n",
		dch->vc.chan.chan_id, dir, buf_len, period_len);

	/* TODO: Implement cyclic DMA for audio */
	return NULL;
}

static int d350_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct d350 *dmac;
	struct clk *clk;
	void __iomem *base;
	u32 reg;
	int ret, nchan, dw, aw, r, p;
	bool coherent, memset;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* Get clock (optional - some platforms may not have one) */
	clk = devm_clk_get_optional(dev, "axiclk");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "Failed to get clock\n");

	/* Fallback to unnamed clock if axiclk not found */
	if (!clk) {
		clk = devm_clk_get_optional(dev, NULL);
		if (IS_ERR(clk))
			return dev_err_probe(dev, PTR_ERR(clk), "Failed to get clock\n");
	}

	/*
	 * Enable runtime PM to activate power domain. The runtime_resume
	 * callback handles NULL dmac gracefully during this initial call.
	 * We then manually enable the clock since dmac isn't set up yet.
	 */
	dev_info(dev, "DMA-350 probe: enabling pm_runtime\n");
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "Failed to power on device\n");
	}
	dev_info(dev, "DMA-350 probe: pm_runtime_resume_and_get returned %d\n", ret);

	/* Manually enable clock for initial probe (runtime_resume skipped it) */
	dev_info(dev, "DMA-350 probe: enabling clock %pC\n", clk);
	ret = clk_prepare_enable(clk);
	if (ret) {
		pm_runtime_put(dev);
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "Failed to enable clock\n");
	}

	/* Get and deassert reset (optional - some platforms may not have one) */
	{
		struct reset_control *rst;

		rst = devm_reset_control_get_optional_exclusive(dev, "dma_reset");
		if (IS_ERR(rst)) {
			ret = PTR_ERR(rst);
			dev_err(dev, "Failed to get reset: %d\n", ret);
			goto err_pm_put;
		}
		if (rst) {
			dev_info(dev, "DMA-350 probe: deasserting reset\n");
			ret = reset_control_deassert(rst);
			if (ret) {
				dev_err(dev, "Failed to deassert reset: %d\n", ret);
				goto err_pm_put;
			}
			/* Small delay after reset deassert */
			usleep_range(10, 20);
		}
	}

	dev_info(dev, "DMA-350 probe: about to read IIDR at %p + 0x%x\n",
		 base, DMAINFO + IIDR);

	reg = readl_relaxed(base + DMAINFO + IIDR);
	r = FIELD_GET(IIDR_VARIANT, reg);
	p = FIELD_GET(IIDR_REVISION, reg);
	if (FIELD_GET(IIDR_IMPLEMENTER, reg) != IMPLEMENTER_ARM ||
	    FIELD_GET(IIDR_PRODUCTID, reg) != PRODUCTID_DMA350) {
		ret = -ENODEV;
		dev_err(dev, "Not a DMA-350!");
		goto err_pm_put;
	}

	reg = readl_relaxed(base + DMAINFO + DMA_BUILDCFG0);
	nchan = FIELD_GET(DMA_CFG_NUM_CHANNELS, reg) + 1;
	dw = 1 << FIELD_GET(DMA_CFG_DATA_WIDTH, reg);
	aw = FIELD_GET(DMA_CFG_ADDR_WIDTH, reg) + 1;

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(aw));
	coherent = device_get_dma_attr(dev) == DEV_DMA_COHERENT;

	dmac = devm_kzalloc(dev, struct_size(dmac, channels, nchan), GFP_KERNEL);
	if (!dmac) {
		ret = -ENOMEM;
		goto err_pm_put;
	}

	dmac->nchan = nchan;
	dmac->clk = clk;
	platform_set_drvdata(pdev, dmac);

	reg = readl_relaxed(base + DMAINFO + DMA_BUILDCFG1);
	dmac->nreq = FIELD_GET(DMA_CFG_NUM_TRIGGER_IN, reg);

	dev_dbg(dev, "DMA-350 r%dp%d with %d channels, %d requests\n", r, p, dmac->nchan, dmac->nreq);

	dmac->dma.dev = dev;
	for (int i = min(dw, 16); i > 0; i /= 2) {
		dmac->dma.src_addr_widths |= BIT(i);
		dmac->dma.dst_addr_widths |= BIT(i);
	}
	dmac->dma.directions = BIT(DMA_MEM_TO_MEM) | BIT(DMA_MEM_TO_DEV) | BIT(DMA_DEV_TO_MEM);
	dmac->dma.descriptor_reuse = true;
	dmac->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;
	dmac->dma.device_alloc_chan_resources = d350_alloc_chan_resources;
	dmac->dma.device_free_chan_resources = d350_free_chan_resources;
	dmac->dma.device_config = d350_device_config;
	dma_cap_set(DMA_MEMCPY, dmac->dma.cap_mask);
	dma_cap_set(DMA_SLAVE, dmac->dma.cap_mask);
	dma_cap_set(DMA_CYCLIC, dmac->dma.cap_mask);
	dma_cap_set(DMA_PRIVATE, dmac->dma.cap_mask);
	dmac->dma.device_prep_dma_memcpy = d350_prep_memcpy;
	dmac->dma.device_prep_slave_sg = d350_prep_slave_sg;
	dmac->dma.device_prep_dma_cyclic = d350_prep_dma_cyclic;
	dmac->dma.device_pause = d350_pause;
	dmac->dma.device_resume = d350_resume;
	dmac->dma.device_terminate_all = d350_terminate_all;
	dmac->dma.device_synchronize = d350_synchronize;
	dmac->dma.device_tx_status = d350_tx_status;
	dmac->dma.device_issue_pending = d350_issue_pending;
	INIT_LIST_HEAD(&dmac->dma.channels);

	/* Would be nice to have per-channel caps for this... */
	memset = true;
	for (int i = 0; i < nchan; i++) {
		struct d350_chan *dch = &dmac->channels[i];

		dch->base = base + DMACH(i);
		writel_relaxed(CH_CMD_CLEAR, dch->base + CH_CMD);

		reg = readl_relaxed(dch->base + CH_BUILDCFG1);
		if (!(FIELD_GET(CH_CFG_HAS_CMDLINK, reg))) {
			dev_warn(dev, "No command link support on channel %d\n", i);
			continue;
		}
		dch->irq = platform_get_irq_optional(pdev, i);
		if (dch->irq < 0) {
			/*
			 * Some platforms (e.g., Sky1 AUDSS) provide only a single
			 * shared interrupt for all channels. Fall back to IRQ 0.
			 */
			if (i > 0) {
				dch->irq = dmac->channels[0].irq;
				dev_dbg(dev, "Channel %d using shared IRQ %d\n",
					i, dch->irq);
			} else {
				return dev_err_probe(dev, dch->irq,
						     "Failed to get IRQ for channel %d\n", i);
			}
		}

		dch->has_wrap = FIELD_GET(CH_CFG_HAS_WRAP, reg);
		dch->has_trig = FIELD_GET(CH_CFG_HAS_TRIGIN, reg) &
				FIELD_GET(CH_CFG_HAS_TRIGSEL, reg);

		/* Fill is a special case of Wrap */
		memset &= dch->has_wrap;

		reg = readl_relaxed(dch->base + CH_BUILDCFG0);
		dch->tsz = FIELD_GET(CH_CFG_DATA_WIDTH, reg);

		reg = FIELD_PREP(CH_LINK_SHAREATTR, coherent ? SHAREATTR_ISH : SHAREATTR_OSH);
		reg |= FIELD_PREP(CH_LINK_MEMATTR, coherent ? MEMATTR_WB : MEMATTR_NC);
		writel_relaxed(reg, dch->base + CH_LINKATTR);

		dch->vc.desc_free = d350_desc_free;
		vchan_init(&dch->vc, &dmac->dma);
	}

	if (memset) {
		dma_cap_set(DMA_MEMSET, dmac->dma.cap_mask);
		dmac->dma.device_prep_dma_memset = d350_prep_memset;
	}

	ret = dma_async_device_register(&dmac->dma);
	if (ret) {
		dev_err(dev, "Failed to register DMA device: %d\n", ret);
		goto err_pm_put;
	}

	/* Register for device tree DMA channel requests */
	ret = of_dma_controller_register(dev->of_node, d350_of_xlate, dmac);
	if (ret) {
		dev_err(dev, "Failed to register OF DMA controller: %d\n", ret);
		goto err_dma_unregister;
	}

	dev_info(dev, "DMA-350 r%dp%d initialized with %d channels\n",
		 r, p, dmac->nchan);
	return 0;

err_dma_unregister:
	dma_async_device_unregister(&dmac->dma);

err_pm_put:
	clk_disable_unprepare(clk);
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
	return ret;
}

static void d350_remove(struct platform_device *pdev)
{
	struct d350 *dmac = platform_get_drvdata(pdev);

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&dmac->dma);
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static int __maybe_unused d350_runtime_suspend(struct device *dev)
{
	struct d350 *dmac = dev_get_drvdata(dev);

	if (dmac && dmac->clk)
		clk_disable_unprepare(dmac->clk);
	return 0;
}

static int __maybe_unused d350_runtime_resume(struct device *dev)
{
	struct d350 *dmac = dev_get_drvdata(dev);

	if (dmac && dmac->clk)
		return clk_prepare_enable(dmac->clk);
	return 0;
}

static const struct dev_pm_ops d350_pm_ops = {
	SET_RUNTIME_PM_OPS(d350_runtime_suspend, d350_runtime_resume, NULL)
};

static const struct of_device_id d350_of_match[] __maybe_unused = {
	{ .compatible = "arm,dma-350" },
	{ .compatible = "arm,dma350-no-pause" },
	{}
};
MODULE_DEVICE_TABLE(of, d350_of_match);

static struct platform_driver d350_driver = {
	.driver = {
		.name = "arm-dma350",
		.of_match_table = of_match_ptr(d350_of_match),
		.pm = &d350_pm_ops,
	},
	.probe = d350_probe,
	.remove = d350_remove,
};
module_platform_driver(d350_driver);

MODULE_AUTHOR("Robin Murphy <robin.murphy@arm.com>");
MODULE_DESCRIPTION("Arm DMA-350 driver");
MODULE_LICENSE("GPL v2");
