#ifndef _ZYNQ_RPROC_H_
#define _ZYNQ_RPROC_H_

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/remoteproc.h>

#define MSIC_START_ADDRESS 0x60000
#define MSIC_CONTROL 0x0
#define MSIC_STATUS 0x4
#define MSIC_ISR 0xc

struct zynq_pcibar {
	void __iomem *addr;
	size_t size;
};

struct zynq_dev {
	struct pci_dev *pdev;
	u32 irq_cnt;
	struct rproc *rproc;
	struct zynq_pcibar plbar;
	struct zynq_pcibar membar;
	struct zynq_pcibar slcrbar;
};

int zynq_rproc_init(struct zynq_dev *zynq);

void zynq_rproc_exit(struct zynq_dev *zynq);

irqreturn_t zynq_rproc_irq(int irq, void *zynq_ptr);

#endif // _ZYNQ_RPROC_H_
