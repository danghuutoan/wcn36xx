/*
 * Contact: Eugene Krasnikov <k.eugene.e@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// DXE - DMA transfer engine
// we have 2 channels(High prio and Low prio) for TX and 2 channels for RX.
// through low channels data packets are transfered
// through high channels managment packets are transfered

#include <linux/interrupt.h>
#include "dxe.h"
#include "txrx.h"
#include "wcn36xx.h"

static void wcn36xx_dxe_write_register(struct wcn36xx *wcn, int addr, int data)
{
	wcn36xx_dbg(WCN36XX_DBG_DXE,
		    "wcn36xx_dxe_write_register: addr=%x, data=%x",
		    addr, data);
	writel(data, wcn->mmio + addr);
}

static void wcn36xx_dxe_read_register(struct wcn36xx *wcn, int addr, int* data)
{
	*data = readl(wcn->mmio + addr);
	wcn36xx_dbg(WCN36XX_DBG_DXE,
		    "wcn36xx_dxe_read_register: addr=%x, data=%x",
		    addr, *data);
}

static int wcn36xx_dxe_allocate_ctl_block(struct wcn36xx_dxe_ch *ch)
{
	struct wcn36xx_dxe_ctl *prev_dxe_ctl = NULL;
	struct wcn36xx_dxe_ctl *cur_dxe_ctl = NULL;
	int i;
	for (i = 0; i < ch->desc_num; i++)
	{
		cur_dxe_ctl = kmalloc(sizeof(*cur_dxe_ctl), GFP_KERNEL);
		if (!cur_dxe_ctl) {
			return -ENOMEM;
		}
		cur_dxe_ctl->frame = NULL;
		cur_dxe_ctl->ctl_blk_order = i;

		ch->tail_blk_ctl = cur_dxe_ctl;
		if (i == 0) {
			ch->head_blk_ctl = cur_dxe_ctl;
		} else if (ch->desc_num - 1 == i){
			prev_dxe_ctl->next = cur_dxe_ctl;
			cur_dxe_ctl->next = ch->head_blk_ctl;
		} else {
			prev_dxe_ctl->next = cur_dxe_ctl;
		}
		prev_dxe_ctl = cur_dxe_ctl;
	}
	return 0;
}

static void wcn36xx_dxe_free_ctl_block(struct wcn36xx_dxe_ch *ch)
{
	struct wcn36xx_dxe_ctl *cur_dxe_ctl = ch->head_blk_ctl, *next;
	while (1) {
		next = cur_dxe_ctl->next;
		kfree(cur_dxe_ctl);
		if (ch->tail_blk_ctl == cur_dxe_ctl)
			break;
		cur_dxe_ctl = next;
	}
}

int wcn36xx_dxe_alloc_ctl_blks(struct wcn36xx *wcn)
{
	int ret = 0;

	wcn->dxe_tx_l_ch.ch_type = WCN36XX_DXE_CH_TX_L;
	wcn->dxe_tx_h_ch.ch_type = WCN36XX_DXE_CH_TX_H;
	wcn->dxe_rx_l_ch.ch_type = WCN36XX_DXE_CH_RX_L;
	wcn->dxe_rx_h_ch.ch_type = WCN36XX_DXE_CH_RX_H;

	wcn->dxe_tx_l_ch.desc_num = WCN36XX_DXE_CH_DESC_NUMB_TX_L;
	wcn->dxe_tx_h_ch.desc_num = WCN36XX_DXE_CH_DESC_NUMB_TX_H;
	wcn->dxe_rx_l_ch.desc_num = WCN36XX_DXE_CH_DESC_NUMB_RX_L;
	wcn->dxe_rx_h_ch.desc_num = WCN36XX_DXE_CH_DESC_NUMB_RX_H;

	wcn->dxe_tx_l_ch.dxe_wq =  WCN36XX_DXE_WQ_TX_L;
	wcn->dxe_tx_h_ch.dxe_wq =  WCN36XX_DXE_WQ_TX_H;

	wcn->dxe_tx_l_ch.ctrl_bd = WCN36XX_DXE_CTRL_TX_L_BD;
	wcn->dxe_tx_h_ch.ctrl_bd = WCN36XX_DXE_CTRL_TX_H_BD;

	wcn->dxe_tx_l_ch.ctrl_skb = WCN36XX_DXE_CTRL_TX_L_SKB;
	wcn->dxe_tx_h_ch.ctrl_skb = WCN36XX_DXE_CTRL_TX_H_SKB;

	wcn->dxe_tx_l_ch.reg_ctrl = WCN36XX_DXE_REG_CTL_TX_L;
	wcn->dxe_tx_h_ch.reg_ctrl = WCN36XX_DXE_REG_CTL_TX_H;

	wcn->dxe_tx_l_ch.def_ctrl = WCN36XX_DXE_CH_DEFAULT_CTL_TX_L;
	wcn->dxe_tx_h_ch.def_ctrl = WCN36XX_DXE_CH_DEFAULT_CTL_TX_H;

	//DEX control block allocation
	//TODO: Error handling
	wcn36xx_dxe_allocate_ctl_block(&wcn->dxe_tx_l_ch);
	wcn36xx_dxe_allocate_ctl_block(&wcn->dxe_tx_h_ch);
	wcn36xx_dxe_allocate_ctl_block(&wcn->dxe_rx_l_ch);
	wcn36xx_dxe_allocate_ctl_block(&wcn->dxe_rx_h_ch);

	// TODO most probably do not need this
	/* Initialize SMSM state  Clear TX Enable RING EMPTY STATE */
	ret = smsm_change_state(SMSM_APPS_STATE,
		WCN36XX_SMSM_WLAN_TX_ENABLE,
		WCN36XX_SMSM_WLAN_TX_RINGS_EMPTY);

	return ret;
}

void wcn36xx_dxe_free_ctl_blks(struct wcn36xx *wcn)
{
	wcn36xx_dxe_free_ctl_block(&wcn->dxe_tx_l_ch);
	wcn36xx_dxe_free_ctl_block(&wcn->dxe_tx_h_ch);
	wcn36xx_dxe_free_ctl_block(&wcn->dxe_rx_l_ch);
	wcn36xx_dxe_free_ctl_block(&wcn->dxe_rx_h_ch);
}

static int wcn36xx_dxe_init_descs(struct wcn36xx_dxe_ch *wcn_ch)
{
	struct wcn36xx_dxe_desc *cur_dxe_desc = NULL;
	struct wcn36xx_dxe_desc *prev_dxe_desc = NULL;
	struct wcn36xx_dxe_ctl *cur_dxe_ctl = NULL;
	size_t size;
	int i;

	size = wcn_ch->desc_num * sizeof(struct wcn36xx_dxe_desc);
	wcn_ch->cpu_addr = dma_alloc_coherent(NULL, size, &wcn_ch->dma_addr,
					      GFP_KERNEL);
	if (!wcn_ch->cpu_addr)
		return -ENOMEM;

	memset(wcn_ch->cpu_addr, 0, size);

	cur_dxe_desc = (struct wcn36xx_dxe_desc *)wcn_ch->cpu_addr;
	cur_dxe_ctl = wcn_ch->head_blk_ctl;

	for (i = 0; i < wcn_ch->desc_num; i++)
	{
		cur_dxe_ctl->desc = cur_dxe_desc;
		cur_dxe_ctl->desc_phy_addr = wcn_ch->dma_addr +
			i * sizeof(struct wcn36xx_dxe_desc);

		switch (wcn_ch->ch_type) {
		case WCN36XX_DXE_CH_TX_L:
			cur_dxe_desc->desc_ctl.ctrl = WCN36XX_DXE_CTRL_TX_L;
			cur_dxe_desc->desc.dst_addr_l = WCN36XX_DXE_WQ_TX_L;
			break;
		case WCN36XX_DXE_CH_TX_H:
			cur_dxe_desc->desc_ctl.ctrl = WCN36XX_DXE_CTRL_TX_H;
			cur_dxe_desc->desc.dst_addr_l = WCN36XX_DXE_WQ_TX_H;
			break;
		case WCN36XX_DXE_CH_RX_L:
			cur_dxe_desc->desc_ctl.ctrl = WCN36XX_DXE_CTRL_RX_L;
			cur_dxe_desc->desc.src_addr_l = WCN36XX_DXE_WQ_RX_L;
			break;
		case WCN36XX_DXE_CH_RX_H:
			cur_dxe_desc->desc_ctl.ctrl = WCN36XX_DXE_CTRL_RX_H;
			cur_dxe_desc->desc.src_addr_l = WCN36XX_DXE_WQ_RX_H;
			break;
		}
		if (0 == i) {
			cur_dxe_desc->desc.phy_next_l = 0;
		} else if ((0 < i) && (i < wcn_ch->desc_num - 1)) {
			prev_dxe_desc->desc.phy_next_l =
				cur_dxe_ctl->desc_phy_addr;
		} else if (i == (wcn_ch->desc_num -1)) {
			prev_dxe_desc->desc.phy_next_l =
				cur_dxe_ctl->desc_phy_addr;
			cur_dxe_desc->desc.phy_next_l =
				wcn_ch->head_blk_ctl->desc_phy_addr;
		}
		cur_dxe_ctl = cur_dxe_ctl->next;
		prev_dxe_desc = cur_dxe_desc;
		cur_dxe_desc++;
	}
	return 0;
}
static int wcn36xx_dxe_enable_ch_int(struct wcn36xx *wcn, u16 wcn_ch)
{
	int reg_data = 0;
	wcn36xx_dxe_read_register(wcn,
		WCN36XX_DXE_INT_MASK_REG,
		&reg_data);
	reg_data |= wcn_ch;
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_INT_MASK_REG,
		(int)reg_data);
	return 0;
}
static int wcn36xx_dxe_ch_alloc_skb(struct wcn36xx *wcn, struct wcn36xx_dxe_ch *wcn_ch)
{
	int i;
	struct wcn36xx_dxe_ctl *cur_dxe_ctl = NULL;
	cur_dxe_ctl = wcn_ch->head_blk_ctl;

	for ( i = 0; i < wcn_ch->desc_num; i++)
	{
		cur_dxe_ctl->skb = alloc_skb(WCN36XX_PKT_SIZE, GFP_KERNEL);
		skb_reserve(cur_dxe_ctl->skb, WCN36XX_PKT_SIZE);
		skb_headroom(cur_dxe_ctl->skb);
		skb_push(cur_dxe_ctl->skb, WCN36XX_PKT_SIZE);
		cur_dxe_ctl->desc->desc.dst_addr_l = dma_map_single(NULL,
			cur_dxe_ctl->skb->data,
			cur_dxe_ctl->skb->len,
			DMA_FROM_DEVICE);
		cur_dxe_ctl = cur_dxe_ctl->next;
	}
	return 0;
}

static void wcn36xx_dxe_ch_free_skbs(struct wcn36xx *wcn,
				     struct wcn36xx_dxe_ch *wcn_ch)
{
	struct wcn36xx_dxe_ctl *cur = wcn_ch->head_blk_ctl;
	int i;

	for (i = 0; i < wcn_ch->desc_num; i++) {
		kfree_skb(cur->skb);
		cur = cur->next;
	}
}

static irqreturn_t wcn36xx_irq_tx_complete(int irq, void *dev)
{
	return IRQ_HANDLED;
}

static irqreturn_t wcn36xx_irq_rx_ready(int irq, void *dev)
{
	struct wcn36xx *wcn = (struct wcn36xx *)dev;
	disable_irq_nosync(wcn->rx_irq);
	queue_work(wcn->ctl_wq, &wcn->rx_ready_work);
	return IRQ_HANDLED;
}
static int wcn36xx_dxe_request_irqs(struct wcn36xx *wcn)
{
	int ret;

	// Register TX complete irq
	ret =request_irq(wcn->tx_irq, wcn36xx_irq_tx_complete, IRQF_TRIGGER_HIGH,
                           "wcn36xx_tx", wcn);
	if (ret) {
		wcn36xx_error("failed to alloc tx irq");
		goto out_err;
	}

	// Register RX irq
	ret = request_irq(wcn->rx_irq, wcn36xx_irq_rx_ready, IRQF_TRIGGER_HIGH,
                           "wcn36xx_rx", wcn);
	if (ret) {
		wcn36xx_error("failed to alloc rx irq");
		goto out_txirq;
	}
	// disable tx irq, not supported
	disable_irq_nosync(wcn->tx_irq);

	// enable rx irq
	enable_irq_wake(wcn->rx_irq);
	return 0;

out_txirq:
	free_irq(wcn->tx_irq, wcn);
out_err:
	return -ret;

}
void wcn36xx_rx_ready_work(struct work_struct *work)
{
	struct wcn36xx *wcn =
		container_of(work, struct wcn36xx, rx_ready_work);
	struct wcn36xx_dxe_desc *cur_dxe_desc = NULL;
	struct wcn36xx_dxe_ctl *cur_dxe_ctl = NULL;
	int intSrc;
	int int_reason;

	// TODO read which channel generated INT by checking mask
	wcn36xx_dxe_read_register(wcn, WCN36XX_DXE_INT_SRC_RAW_REG, &intSrc);

	wcn36xx_dbg(WCN36XX_DBG_DXE, "dxe rx ready channel %x %s%s",
		    intSrc,
		    intSrc & WCN36XX_INT_MASK_CHAN_RX_H ? "high " : "",
		    intSrc & WCN36XX_INT_MASK_CHAN_RX_L ? "low " : "");

	// check if this channel is High or Low. Assume high
	if (intSrc & WCN36XX_INT_MASK_CHAN_RX_H) {
		/* Read Channel Status Register to know why INT Happen */
		wcn36xx_dxe_read_register(wcn, WCN36XX_DXE_CH_STATUS_REG_ADDR_RX_H, &int_reason);
		// TODO if status says erro handle that

		/* Clean up all the INT within this channel */
		wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_INT_CLR_ADDR ,  WCN36XX_INT_MASK_CHAN_RX_H);

		/* Clean up ED INT Bit */
		wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_INT_END_CLR_ADDR, WCN36XX_INT_MASK_CHAN_RX_H);

		cur_dxe_ctl = wcn->dxe_rx_h_ch.head_blk_ctl;
		cur_dxe_desc = cur_dxe_ctl->desc;

		wcn36xx_dbg(WCN36XX_DBG_DXE,
			    "dxe rx ready order %d ctl %x",
			    cur_dxe_ctl->ctl_blk_order,
			    cur_dxe_desc->desc_ctl.ctrl);

		dma_unmap_single( NULL,
			(dma_addr_t)cur_dxe_desc->desc.dst_addr_l,
			cur_dxe_ctl->skb->len,
			DMA_FROM_DEVICE );
		wcn36xx_rx_skb(wcn, cur_dxe_ctl->skb);

		// Release RX descriptor
		cur_dxe_desc->desc_ctl.ctrl = WCN36XX_DXE_CTRL_RX_H;

		wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_ENCH_ADDR, WCN36XX_INT_MASK_CHAN_RX_H);

		// Reenable RX Low not high
		wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_REG_CTL_RX_L, WCN36XX_DXE_CH_DEFAULT_CTL_RX_L);

		wcn->dxe_rx_h_ch.head_blk_ctl = cur_dxe_ctl->next;
	} else if (intSrc & WCN36XX_INT_MASK_CHAN_RX_L) {
		/* Read Channel Status Register to know why INT Happen */
		wcn36xx_dxe_read_register(wcn, WCN36XX_DXE_CH_STATUS_REG_ADDR_RX_L, &int_reason);
		// TODO if status says erro handle that

		/* Clean up all the INT within this channel */
		wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_INT_CLR_ADDR ,  WCN36XX_INT_MASK_CHAN_RX_L);

		/* Clean up ED INT Bit */
		wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_INT_END_CLR_ADDR, WCN36XX_INT_MASK_CHAN_RX_L);

		cur_dxe_ctl = wcn->dxe_rx_l_ch.head_blk_ctl;
		cur_dxe_desc = cur_dxe_ctl->desc;

		wcn36xx_dbg(WCN36XX_DBG_DXE,
			    "dxe rx ready order %d ctl %x",
			    cur_dxe_ctl->ctl_blk_order,
			    cur_dxe_desc->desc_ctl.ctrl);

		dma_unmap_single( NULL,
			(dma_addr_t)cur_dxe_desc->desc.dst_addr_l,
			cur_dxe_ctl->skb->len,
			DMA_FROM_DEVICE );
		wcn36xx_rx_skb(wcn, cur_dxe_ctl->skb);

		// Release RX descriptor
		cur_dxe_desc->desc_ctl.ctrl = WCN36XX_DXE_CTRL_RX_L;

		wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_ENCH_ADDR, WCN36XX_INT_MASK_CHAN_RX_L);

		// Reenable RX Low not high
		wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_REG_CTL_RX_H, WCN36XX_DXE_CH_DEFAULT_CTL_RX_H);

		wcn->dxe_rx_l_ch.head_blk_ctl = cur_dxe_ctl->next;
	}

	enable_irq(wcn->rx_irq);
}
int wcn36xx_dxe_allocate_mem_pools(struct wcn36xx *wcn)
{
	size_t s;
	void *cpu_addr;

	/* Allocate BD headers for MGMT frames */

	// Where this come from ask QC
	wcn->mgmt_mem_pool.chunk_size =	WCN36XX_BD_CHUNK_SIZE +
		16 - (WCN36XX_BD_CHUNK_SIZE % 8);

	s = wcn->mgmt_mem_pool.chunk_size * WCN36XX_DXE_CH_DESC_NUMB_TX_H;
	cpu_addr = dma_alloc_coherent(NULL, s, &wcn->mgmt_mem_pool.phy_addr,
				      GFP_KERNEL);

	wcn->mgmt_mem_pool.virt_addr = cpu_addr;
	memset(cpu_addr, 0, s);
	wcn->mgmt_mem_pool.bitmap =
		kzalloc((WCN36XX_DXE_CH_DESC_NUMB_TX_H / 32 + 1) *
		sizeof(u32), GFP_KERNEL);

	/* Allocate BD headers for DATA frames */

	// Where this come from ask QC
	wcn->data_mem_pool.chunk_size = WCN36XX_BD_CHUNK_SIZE +
		16 - (WCN36XX_BD_CHUNK_SIZE % 8);

	s = wcn->data_mem_pool.chunk_size * WCN36XX_DXE_CH_DESC_NUMB_TX_L;
	cpu_addr = dma_alloc_coherent(NULL, s, &wcn->data_mem_pool.phy_addr,
				      GFP_KERNEL);
	wcn->data_mem_pool.virt_addr = cpu_addr;
	memset(cpu_addr, 0, s);
	wcn->data_mem_pool.bitmap =
		kzalloc((WCN36XX_DXE_CH_DESC_NUMB_TX_L / 32 + 1) *
		sizeof(u32), GFP_KERNEL);
	return 0;
}

void wcn36xx_dxe_free_mem_pools(struct wcn36xx *wcn)
{
	if (wcn->mgmt_mem_pool.virt_addr)
		dma_free_coherent(NULL, wcn->mgmt_mem_pool.chunk_size *
				  WCN36XX_DXE_CH_DESC_NUMB_TX_H,
				  wcn->mgmt_mem_pool.virt_addr,
				  wcn->mgmt_mem_pool.phy_addr);
	if (wcn->data_mem_pool.virt_addr) {
		dma_free_coherent(NULL, wcn->data_mem_pool.chunk_size *
				  WCN36XX_DXE_CH_DESC_NUMB_TX_L,
				  wcn->data_mem_pool.virt_addr,
				  wcn->data_mem_pool.phy_addr);
	}
	kfree(wcn->data_mem_pool.bitmap);
	kfree(wcn->mgmt_mem_pool.bitmap);
}

int wcn36xx_dxe_tx(struct wcn36xx *wcn,
		   struct sk_buff *skb,
		   u8 broadcast,
		   bool is_high,
		   u32 header_len)
{
	struct wcn36xx_dxe_ctl *cur_dxe_ctl = NULL;
	struct wcn36xx_dxe_desc *cur_dxe_desc = NULL;
	struct wcn36xx_dxe_mem_pool * mem_pool = NULL;
	struct wcn36xx_dxe_ch * cur_ch = NULL;

	if(is_high) {
		mem_pool = &wcn->mgmt_mem_pool;
		cur_ch = &wcn->dxe_tx_h_ch;
	} else {
		mem_pool = &wcn->data_mem_pool;
		cur_ch = &wcn->dxe_tx_l_ch;
	}

	wcn36xx_prepare_tx_bd(mem_pool->virt_addr, skb->len, header_len);
	if (!is_high && WCN36XX_BSS_KEY == wcn->en_state) {
		wcn36xx_dbg(WCN36XX_DBG_DXE, "DXE Encription enabled");
		wcn36xx_fill_tx_bd(wcn, mem_pool->virt_addr, broadcast, 0);
	} else {
		wcn36xx_fill_tx_bd(wcn, mem_pool->virt_addr, broadcast, 1);
	}

	cur_dxe_ctl = cur_ch->head_blk_ctl;
	cur_dxe_desc = cur_dxe_ctl->desc;

	// Let's not forget the frame we are sending
	cur_dxe_ctl->frame = mem_pool->virt_addr;

	// Set source address of the BD we send
	cur_dxe_desc->desc.src_addr_l = (int)mem_pool->phy_addr;

	cur_dxe_desc->desc.dst_addr_l = cur_ch->dxe_wq;
	cur_dxe_desc->fr_len = sizeof(struct wcn36xx_tx_bd);
	cur_dxe_desc->desc_ctl.ctrl = cur_ch->ctrl_bd;

	wcn36xx_dbg(WCN36XX_DBG_DXE, "DXE TX");

	wcn36xx_dbg_dump(WCN36XX_DBG_DXE_DUMP, "DESC1 >>> ",
			 (char*)cur_dxe_desc, sizeof(*cur_dxe_desc));
	wcn36xx_dbg_dump(WCN36XX_DBG_DXE_DUMP,
			 "BD   >>> ", (char*)mem_pool->virt_addr,
			 sizeof(struct wcn36xx_tx_bd));

	// Set source address of the SKB we send
	cur_dxe_ctl = (struct wcn36xx_dxe_ctl*)cur_dxe_ctl->next;
	cur_dxe_ctl->skb = skb;
	cur_dxe_desc = cur_dxe_ctl->desc;
	cur_dxe_desc->desc.src_addr_l = (int)dma_map_single(NULL,
		cur_dxe_ctl->skb->data,
		cur_dxe_ctl->skb->len,
		DMA_TO_DEVICE );

	cur_dxe_desc->desc.dst_addr_l = cur_ch->dxe_wq;
	cur_dxe_desc->fr_len = cur_dxe_ctl->skb->len;

	// set it to VALID
	cur_dxe_desc->desc_ctl.ctrl = cur_ch->ctrl_skb;

	wcn36xx_dbg_dump(WCN36XX_DBG_DXE_DUMP, "DESC2 >>> ",
			 (char*)cur_dxe_desc, sizeof(*cur_dxe_desc));
	wcn36xx_dbg_dump(WCN36XX_DBG_DXE_DUMP, "SKB   >>> ",
			 (char*)cur_dxe_ctl->skb->data, cur_dxe_ctl->skb->len);

	// Move the head of the ring to the next empty descriptor
	 cur_ch->head_blk_ctl = cur_dxe_ctl->next;

	//indicate End Of Packet and generate interrupt on descriptor Done
	wcn36xx_dxe_write_register(wcn,
		cur_ch->reg_ctrl, cur_ch->def_ctrl);
	return 0;
}
int wcn36xx_dxe_init(struct wcn36xx *wcn)
{
	int reg_data = 0, ret;

	reg_data = WCN36XX_DXE_REG_RESET;
	wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_REG_CSR_RESET, reg_data);

	// Setting interrupt path
	reg_data = WCN36XX_DXE_CCU_INT;
	wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_REG_CCU_INT, reg_data);

	/***************************************/
	/* Init descriptors for TX LOW channel */
	/***************************************/
	wcn36xx_dxe_init_descs(&wcn->dxe_tx_l_ch);

	// Write chanel head to a NEXT register
	wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_CH_NEXT_DESC_ADDR_TX_L,
		wcn->dxe_tx_l_ch.head_blk_ctl->desc_phy_addr);

	// Program DMA destination addr for TX LOW
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_CH_DEST_ADDR_TX_L,
		WCN36XX_DXE_WQ_TX_L);

	wcn36xx_dxe_read_register(wcn, WCN36XX_DXE_REG_CH_EN, &reg_data);
	wcn36xx_dxe_enable_ch_int(wcn, WCN36XX_INT_MASK_CHAN_TX_L);

	/***************************************/
	/* Init descriptors for TX HIGH channel */
	/***************************************/
	wcn36xx_dxe_init_descs(&wcn->dxe_tx_h_ch);

	// Write chanel head to a NEXT register
	wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_CH_NEXT_DESC_ADDR_TX_H,
		wcn->dxe_tx_h_ch.head_blk_ctl->desc_phy_addr);

	// Program DMA destination addr for TX HIGH
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_CH_DEST_ADDR_TX_H,
		WCN36XX_DXE_WQ_TX_H);

	wcn36xx_dxe_read_register(wcn, WCN36XX_DXE_REG_CH_EN, &reg_data);
	// Enable channel interrupts
	wcn36xx_dxe_enable_ch_int(wcn, WCN36XX_INT_MASK_CHAN_TX_H);

	/***************************************/
	/* Init descriptors for RX LOW channel */
	/***************************************/
	wcn36xx_dxe_init_descs(&wcn->dxe_rx_l_ch);

	// For RX we need to prealocat buffers
	wcn36xx_dxe_ch_alloc_skb(wcn, &wcn->dxe_rx_l_ch);

	// Write chanel head to a NEXT register
	wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_CH_NEXT_DESC_ADDR_RX_L,
		wcn->dxe_rx_l_ch.head_blk_ctl->desc_phy_addr);

	// Write DMA source address
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_CH_SRC_ADDR_RX_L,
		WCN36XX_DXE_WQ_RX_L);

	// Program preallocated destionatio Address
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_CH_DEST_ADDR_RX_L,
		wcn->dxe_rx_l_ch.head_blk_ctl->desc->desc.phy_next_l);


	// Enable default control registers
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_REG_CTL_RX_L,
		WCN36XX_DXE_CH_DEFAULT_CTL_RX_L);

	// Enable channel interrupts
	wcn36xx_dxe_enable_ch_int(wcn, WCN36XX_INT_MASK_CHAN_RX_L);

	/***************************************/
	/* Init descriptors for RX HIGH channel */
	/***************************************/
	wcn36xx_dxe_init_descs(&wcn->dxe_rx_h_ch);

	// For RX we need to prealocat buffers
	wcn36xx_dxe_ch_alloc_skb(wcn, &wcn->dxe_rx_h_ch);

	// Write chanel head to a NEXT register
	wcn36xx_dxe_write_register(wcn, WCN36XX_DXE_CH_NEXT_DESC_ADDR_RX_H,
		wcn->dxe_rx_h_ch.head_blk_ctl->desc_phy_addr);

	// Write DMA source address
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_CH_SRC_ADDR_RX_H,
		WCN36XX_DXE_WQ_RX_H);

	// Program preallocated destionatio Address
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_CH_DEST_ADDR_RX_H,
		 wcn->dxe_rx_h_ch.head_blk_ctl->desc->desc.phy_next_l);

	// Enable default control registers
	wcn36xx_dxe_write_register(wcn,
		WCN36XX_DXE_REG_CTL_RX_H,
		WCN36XX_DXE_CH_DEFAULT_CTL_RX_H);

	// Enable channel interrupts
	wcn36xx_dxe_enable_ch_int(wcn, WCN36XX_INT_MASK_CHAN_RX_H);

	ret = wcn36xx_dxe_request_irqs(wcn);
	if (ret < 0)
		goto out_err;
	return 0;

out_err:
	return ret;
}

void wcn36xx_dxe_deinit(struct wcn36xx *wcn)
{
	free_irq(wcn->tx_irq, wcn);
	free_irq(wcn->rx_irq, wcn);
	wcn36xx_dxe_ch_free_skbs(wcn, &wcn->dxe_rx_l_ch);
	wcn36xx_dxe_ch_free_skbs(wcn, &wcn->dxe_rx_h_ch);
}
