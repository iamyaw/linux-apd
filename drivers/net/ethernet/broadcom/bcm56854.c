#include <linux/device.h>
#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/kobject.h>

MODULE_AUTHOR("Dustin Byford");
MODULE_DESCRIPTION("Broadcom Trident 2 Switch ASIC Driver");
MODULE_LICENSE("GPL");

struct bcm56854_data {
	void *foo;
};

static const struct pci_device_id bcm56854_pci_ids[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, PCI_DEVICE_ID_BCM56854)},
};


static int bcm56854_probe(struct pci_dev *pdev,
			  const struct pci_device_id *ent)
{
	struct bcm56854_data *data;

	dev_info(&pdev->dev, "bcm56854_probe()\n");

	data = devm_kzalloc(&pdev->dev, sizeof *data, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	pci_set_drvdata(pdev, data);

	return 0;
}

static void bcm56854_remove(struct pci_dev *pdev)
{
	//struct bcm56854_data *data = pci_get_drvdata(pdev);
	dev_info(&pdev->dev, "removed\n");
}

static struct pci_driver bcm56854_driver = {
	.name		= "bcm56854",
	.id_table	= bcm56854_pci_ids,
	.probe		= bcm56854_probe,
	.remove		= bcm56854_remove,
};

module_pci_driver(bcm56854_driver);
