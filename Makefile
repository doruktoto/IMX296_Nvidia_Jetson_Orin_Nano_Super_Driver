KDIR ?= /lib/modules/$(shell uname -r)/build
DTC  ?= dtc

# NVIDIA OOT include path (for media/camera_common.h etc. if needed later)
EXTRA_CFLAGS += -I/usr/src/nvidia/nvidia-oot/include

obj-m := imx296.o

all: modules dtbo

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# -@ preserves node labels as __symbols__ so fdtoverlay can resolve cross-references
dtbo: imx296_cam0_overlay.dtbo

imx296_cam0_overlay.dtbo: imx296_cam0_overlay.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f imx296_cam0_overlay.dtbo

KVER  := $(shell uname -r)
MODDIR := /lib/modules/$(KVER)/extra

install:
	install -d $(MODDIR)
	install -m 644 imx296.ko $(MODDIR)/
	depmod -a
	install -m 644 imx296_cam0_overlay.dtbo /boot/
	@echo ""
	@echo "Module installed to $(MODDIR)/imx296.ko"
	@echo "Overlay installed to /boot/imx296_cam0_overlay.dtbo"
