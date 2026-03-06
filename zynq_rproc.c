#include "zynq_rproc.h"

#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>
#include <linux/firmware.h>
#include <linux/elf.h>
#include <linux/reset.h>

#include "remoteproc_internal.h"

#define INTC_START_ADDRESS 0x10000

#define SLCR_UNLOCK 0x8
#define SLCR_UNLOCK_KEY 0xDF0D
#define SLCR_LOCK 0x4
#define SLCR_LOCK_KEY 0x767B
#define A9_CPU_RST_CTRL 0x244
#define A9_RST0_BIT BIT(0)
#define A9_RST1_BIT BIT(1)
#define A9_CLKSTOP0_BIT BIT(4)
#define A9_CLKSTOP1_BIT BIT(5)

#define RS_AWDT_CTRL 0x24C
#define RS_AWDT_CTRL0_BIT BIT(0)
#define RS_AWDT_CTRL1_BIT BIT(1)

#define PSS_IDCODE 0x00000530

#define VRING_SIZE 0x2000
#define SHM_SIZE 0x8000

#define SHM_MIN_SIZE (SHM_SIZE + 2 * VRING_SIZE)

struct zynq_rproc {
	struct zynq_dev *zynq;

	struct delayed_work vq_poll;
	bool ready;
};

static inline u32 zynq_pl_reg_read(struct zynq_dev *zynq, unsigned reg)
{
	return ioread32(zynq->plbar.addr + reg);
}

static inline void zynq_pl_reg_write(struct zynq_dev *zynq, unsigned reg,
				     u32 value)
{
	iowrite32(value, zynq->plbar.addr + reg);
}

static inline u32 zynq_slcr_reg_read(struct zynq_dev *zynq, unsigned reg)
{
	return ioread32(zynq->slcrbar.addr + reg);
}

static inline void zynq_slcr_reg_write(struct zynq_dev *zynq, unsigned reg,
				       u32 value)
{
	iowrite32(value, zynq->slcrbar.addr + reg);
}

static inline u32 intc_read(struct zynq_dev *zynq, u32 reg)
{
	return zynq_pl_reg_read(zynq, INTC_START_ADDRESS + reg);
}

static inline void intc_write(struct zynq_dev *zynq, u32 reg, u32 val)
{
	zynq_pl_reg_write(zynq, INTC_START_ADDRESS + reg, val);
}

static void zynq_slcr_unlock(struct zynq_dev *zynq)
{
	zynq_slcr_reg_write(zynq, SLCR_UNLOCK, SLCR_UNLOCK_KEY);
}

static void zynq_slcr_lock(struct zynq_dev *zynq)
{
	zynq_slcr_reg_write(zynq, SLCR_LOCK, SLCR_LOCK_KEY);
}

/* reset sequence is described in Zynq-7000 SoC Technical Reference Manual
 * UG585 (v1.13) April 2, 2021 Page 109
 */
static void zynq_start(struct zynq_dev *zynq)
{
	u32 ctrl;

	zynq_slcr_unlock(zynq);

	ctrl = zynq_slcr_reg_read(zynq, A9_CPU_RST_CTRL);

	dev_info(&zynq->pdev->dev, "zynq_start A9_CPU_RST_CTRL=%x\n", ctrl);

	/* release reset to CPU0 */
	ctrl &= ~A9_RST0_BIT;

	zynq_slcr_reg_write(zynq, A9_CPU_RST_CTRL, ctrl);

	/* restart clock to CPU0 */
	ctrl &= ~A9_CLKSTOP0_BIT;

	zynq_slcr_reg_write(zynq, A9_CPU_RST_CTRL, ctrl);

	zynq_slcr_lock(zynq);
}

static void zynq_stop(struct zynq_dev *zynq)
{
	u32 ctrl;

	zynq_slcr_unlock(zynq);

	ctrl = zynq_slcr_reg_read(zynq, A9_CPU_RST_CTRL);

	dev_info(&zynq->pdev->dev, "zynq_stop A9_CPU_RST_CTRL=%x\n", ctrl);

	/* reset CPU only */
	zynq_slcr_reg_write(zynq, RS_AWDT_CTRL,
			    RS_AWDT_CTRL0_BIT | RS_AWDT_CTRL1_BIT);

	/* assert reset to CPU */
	ctrl |= A9_RST0_BIT;

	zynq_slcr_reg_write(zynq, A9_CPU_RST_CTRL, ctrl);

	/* stop clock to CPUs */
	ctrl |= A9_CLKSTOP0_BIT;

	zynq_slcr_reg_write(zynq, A9_CPU_RST_CTRL, ctrl);

	zynq_slcr_lock(zynq);
}

static void *zynq_da_to_va(struct zynq_dev *zynq, u64 da)
{
	return zynq->membar.addr + da;
}

static void *zynq_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len,
				 bool *is_iomem)
{
	struct zynq_rproc *zynq_rproc = rproc->priv;
	struct zynq_dev *zynq = zynq_rproc->zynq;

	dev_info(&rproc->dev, "zynq_rproc_da_to_va 0x%llx 0x%lx \n", da, len);

	if (da + len > zynq->membar.size) {
		dev_info(&rproc->dev,
			 "zynq_rproc_da_to_va error size da=0x%llx\n", da);
		return NULL;
	}

	if (is_iomem)
		*is_iomem = true;

	return zynq_da_to_va(zynq, da);
}

static int zynq_rproc_prepare(struct rproc *rproc)
{
	struct zynq_rproc *zynq_rproc = rproc->priv;

	zynq_stop(zynq_rproc->zynq);
	dev_info(&rproc->dev, "zynq_rproc_prepare\n");
	return 0;
}

static int zynq_rproc_unprepare(struct rproc *rproc)
{
	dev_info(&rproc->dev, "zynq_rproc_unprepare\n");
	return 0;
}

static dma_addr_t zynq_va_to_pa(void *va, size_t len)
{
	struct scatterlist sg;
	sg_init_table(&sg, 1);
	sg_set_page(&sg, vmalloc_to_page(va), len, offset_in_page(va));
	return sg_phys(&sg);
}

static int zynq_rproc_start(struct rproc *rproc)
{
	struct zynq_rproc *zynq_rproc = rproc->priv;

	zynq_start(zynq_rproc->zynq);

	intc_write(zynq_rproc->zynq, 0x1c, 0x1);

	u32 reg = intc_read(zynq_rproc->zynq, 0x1c);
	dev_info(&rproc->dev, "zynq_rproc_started 0x%x\n", reg);

	/*schedule_delayed_work(&zynq_rproc->vq_poll, msecs_to_jiffies(5000)); */

	return 0;
}

static int zynq_rproc_stop(struct rproc *rproc)
{
	dev_info(&rproc->dev, "zynq_rproc_stop not stopped!\n");

	struct zynq_rproc *zynq_rproc = rproc->priv;

	zynq_stop(zynq_rproc->zynq);

	cancel_delayed_work_sync(&zynq_rproc->vq_poll);

	zynq_rproc->ready = false;

	dev_info(&rproc->dev, "zynq_rproc_stop\n");
	return 0;
}

static int zynq_rproc_attach(struct rproc *rproc)
{
	dev_info(&rproc->dev, "zynq_rproc_attach\n");
	return 0;
}

static int zynq_rproc_detach(struct rproc *rproc)
{
	struct zynq_rproc *zynq_rproc = rproc->priv;

	cancel_delayed_work_sync(&zynq_rproc->vq_poll);

	dev_info(&rproc->dev, "zynq_rproc_detach\n");
	return 0;
}

static void zynq_rproc_kick(struct rproc *rproc, int vqid)
{
	struct zynq_rproc *zynq_rproc = rproc->priv;
	dev_info(&rproc->dev, "zynq_rproc_kick %x\n", zynq_rproc->ready);

	if (zynq_rproc->ready) {
		/*u32 reg = intc_read(zynq_rproc->zynq); */

		intc_write(zynq_rproc->zynq, 0, 0x1);
	}
}

static void zynq_poll_vq(struct work_struct *work)
{
	struct zynq_rproc *zynq_rproc =
		container_of(work, struct zynq_rproc, vq_poll.work);
	struct rproc *rproc = zynq_rproc->zynq->rproc;
	dev_info(&rproc->dev, "zynq_poll_vq \n");
	if (!zynq_rproc->ready) {
		zynq_rproc->ready = true;
		zynq_rproc_kick(rproc, 0);
	}

	rproc_vq_interrupt(rproc, 0);
	rproc_vq_interrupt(rproc, 1);
}

/**
 * zynq_rproc_alloc_memory() - allocated specified memory
 * @rproc: rproc handle
 * @mem: the memory entry to allocate
 *
 * This function allocate specified memory entry @mem using
 * dma_alloc_coherent() as default allocator
 *
 * Return: 0 on success, or an appropriate error code otherwise
 */
static int zynq_rproc_alloc_memory(struct rproc *rproc,
				   struct rproc_mem_entry *mem)
{
	struct device *dev = &rproc->dev;
	void *va;

	va = zynq_rproc_da_to_va(rproc, mem->da, mem->len, NULL);
	if (!va)
		return -ENOMEM;

	mem->dma = DMA_MAPPING_ERROR;
	mem->va = va;
	mem->is_iomem = true;

	dev_info(dev, "zynq_rproc_alloc_memory %s da=0x%x  len=%ld\n",
		 mem->name, mem->da, mem->len);

	return 0;
}

/**
 * zynq_rproc_release_memory() - release acquired memory
 * @rproc: rproc handle
 * @mem: the memory entry to release
 *
 * This function releases specified memory entry @mem allocated via
 * rproc_alloc_carveout() function by @rproc.
 *
 * Return: 0 on success, or an appropriate error code otherwise
 */
static int zynq_rproc_release_memory(struct rproc *rproc,
				     struct rproc_mem_entry *mem)
{
	return 0;
}

static int iter_vdev(struct device *dev, void *data)
{
	if (dev_is_platform(dev)) {
		struct platform_device *pdev = to_platform_device(dev);
		/* dev_info(dev, "iter_vdev: %s\n", dev->);
		   if (strcmp(pdev->name, "rproc-virtio") == 0) {
		} */
		dev_info(dev, "iter_vdev: platform devie %s\n", pdev->name);
	} else {
		dev_info(dev, "iter_vdev: no platform devie\n");
	}

	return 0;
}

static int zynq_rproc_fixup_rsc(struct rproc *rproc)
{
	struct device *dev = &rproc->dev;
	struct rproc_mem_entry *carveout = NULL;
	device_for_each_child(dev, NULL, iter_vdev);

	dev_info(dev, "zynq_rproc_process_resources\n");

	list_for_each_entry(carveout, &rproc->carveouts, node) {
		dev_info(dev, "zynq_rproc_process_resources carveout: %s\n",
			 carveout->name);
		carveout->alloc = zynq_rproc_alloc_memory;
		carveout->release = zynq_rproc_release_memory;
	}

	return 0;
}

static const struct rproc_ops zynq_rproc_ops = {
	.prepare = zynq_rproc_prepare,
	.unprepare = zynq_rproc_unprepare,
	.start = zynq_rproc_start,
	.stop = zynq_rproc_stop,
	.attach = zynq_rproc_attach,
	.detach = zynq_rproc_detach,
	.kick = zynq_rproc_kick,
	.da_to_va = zynq_rproc_da_to_va,
	.load = rproc_elf_load_segments,
	.parse_fw = rproc_elf_load_rsc_table,
	.fixup_rsc = zynq_rproc_fixup_rsc,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.sanity_check = rproc_elf_sanity_check,
	.get_boot_addr = rproc_elf_get_boot_addr,
};

int zynq_rproc_init(struct zynq_dev *zynq)
{
	struct device *dev = &zynq->pdev->dev;
	struct zynq_rproc *zynq_rproc;
	uint32_t pss_id;
	int ret;

	zynq->rproc = rproc_alloc(dev, "zynqrpoc", &zynq_rproc_ops,
				  "zephyr_zynq.elf", sizeof(struct zynq_rproc));

	if (!zynq->rproc)
		return -ENOMEM;

	zynq_rproc = zynq->rproc->priv;
	zynq_rproc->zynq = zynq;

	zynq->rproc->auto_boot = false;

	/* iommu is al handled by pcie */
	zynq->rproc->has_iommu = false;

	rproc_coredump_set_elf_info(zynq->rproc, ELFCLASS32, EM_NONE);

	ret = rproc_add(zynq->rproc);
	if (ret)
		goto free_rproc;

	INIT_DELAYED_WORK(&zynq_rproc->vq_poll, zynq_poll_vq);

	pss_id = zynq_slcr_reg_read(zynq, PSS_IDCODE);
	dev_info(dev, "pss_id =%x\n", pss_id);

	return 0;

free_rproc:
	rproc_free(zynq->rproc);
	return ret;
}

void zynq_rproc_exit(struct zynq_dev *zynq)
{
	struct zynq_rproc *zynq_rproc = zynq->rproc->priv;
	cancel_delayed_work_sync(&zynq_rproc->vq_poll);
	rproc_del(zynq->rproc);
	rproc_free(zynq->rproc);
}

irqreturn_t zynq_rproc_irq(int irq, void *zynq_ptr)
{
	struct zynq_dev *zynq = zynq_ptr;
	struct zynq_rproc *zynq_rproc = zynq->rproc->priv;

	schedule_delayed_work(&zynq_rproc->vq_poll, 0);

	return IRQ_HANDLED;
}
