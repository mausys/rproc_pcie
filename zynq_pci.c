#include "zynq_rproc.h"

#define ZYNQ_NAME "zynq"
#define ZYNQ_DEVICE_NAME "AMD Zynq PCIe driver"
#define ZYNQ_ENTITY_NAME "zynq"

#define PCI_VENDOR_ID_ZYNQ 0x10ee
#define PCI_DEVICE_ID_ZYNQ 0x7011

#define ZYNQ_PCI_BAR_MEM 0
#define ZYNQ_PCI_BAR_PL 1
#define ZYNQ_PCI_BAR_SLCR 2

#define ZYNQ_NUM_IRQS 1
#define ZYNQ_RPROC_MSI_NR 0

static void zynq_exit_irq(struct zynq_dev *zynq)
{
	struct pci_dev *pdev = zynq->pdev;

	free_irq(pci_irq_vector(pdev, ZYNQ_RPROC_MSI_NR), zynq);

	pci_free_irq_vectors(pdev);
}

static int zynq_init_irq(struct zynq_dev *zynq)
{
	struct pci_dev *pdev = zynq->pdev;
	struct device *dev = &pdev->dev;
	int r;

	r = pci_alloc_irq_vectors(pdev, ZYNQ_NUM_IRQS, ZYNQ_NUM_IRQS,
				  PCI_IRQ_MSI);

	dev_info(dev, "pci_alloc_irq_vectors %d\n", r);

	if (r != ZYNQ_NUM_IRQS) {
		return -EIO;
	}

	r = request_irq(pci_irq_vector(pdev, ZYNQ_RPROC_MSI_NR), zynq_rproc_irq,
			IRQF_SHARED, "zynq_irq_rproc", zynq);
	if (r) {
		zynq_exit_irq(zynq);
		return -EIO;
	}

	return 0;
}

static void zynq_pci_remove(struct pci_dev *pdev)
{
	struct zynq_dev *zynq = pci_get_drvdata(pdev);

	zynq_exit_irq(zynq);

	zynq_rproc_exit(zynq);
}

static int zynq_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct zynq_dev *zynq;
	int r;

	zynq = devm_kzalloc(&pdev->dev, sizeof(struct zynq_dev), GFP_KERNEL);
	if (!zynq)
		return -ENOMEM;

	zynq->pdev = pdev;

	r = pcim_enable_device(pdev);
	if (r) {
		dev_err(dev, "failed to enable device (%d)\n", r);
		return r;
	}

	dev_info(dev, "device 0x%x (rev: 0x%x)\n", pdev->device,
		 pdev->revision);

	r = pcim_iomap_regions(pdev, 7, pci_name(pdev));
	if (r) {
		dev_err(dev, "failed to remap I/O memory (%d)\n", r);
		return -ENODEV;
	}

	pci_set_drvdata(pdev, zynq);

	pci_set_master(pdev);

	r = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (r) {
		dev_err(dev, "failed to set DMA mask (%d)\n", r);
		return -ENODEV;
	}

	zynq->membar.addr = pcim_iomap_table(pdev)[ZYNQ_PCI_BAR_MEM];
	zynq->membar.size = pci_resource_len(pdev, ZYNQ_PCI_BAR_MEM);

	zynq->plbar.addr = pcim_iomap_table(pdev)[ZYNQ_PCI_BAR_PL];
	zynq->plbar.size = pci_resource_len(pdev, ZYNQ_PCI_BAR_PL);

	zynq->slcrbar.addr = pcim_iomap_table(pdev)[ZYNQ_PCI_BAR_SLCR];
	zynq->slcrbar.size = pci_resource_len(pdev, ZYNQ_PCI_BAR_SLCR);

	dev_info(dev,
		 "membar size: 0x%zx plbar size: 0x%zx slcrbar size: 0x%zx\n",
		 zynq->membar.size, zynq->plbar.size, zynq->slcrbar.size);

	r = zynq_init_irq(zynq);

	if (r) {
		dev_err(dev, "zynq_init_irq failed (%d)\n", r);
		return r;
	}

	r = zynq_rproc_init(zynq);

	if (r)
		goto fail;

	return 0;

fail:
	return r;
}

static const struct pci_device_id zynq_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_ZYNQ, PCI_DEVICE_ID_ZYNQ) },
	{
		0,
	}
};

static struct pci_driver zynq_pci_driver = {
	.name = "zynq_pci",
	.id_table = zynq_pci_tbl,
	.probe = zynq_pci_probe,
	.remove = zynq_pci_remove,
};

module_pci_driver(zynq_pci_driver);
MODULE_AUTHOR("Simon Maurer <mail@maurer.systems>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zynq PCIe driver");
