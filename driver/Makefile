# Out-of-tree module build. Normally invoked through scripts/build_module.sh,
# which also copes with repository paths that contain spaces (Kbuild cannot).
#
#   make KDIR=/path/to/prepared/kernel/tree
KDIR ?= /lib/modules/$(shell uname -r)/build

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: modules clean
