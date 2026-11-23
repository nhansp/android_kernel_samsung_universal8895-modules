# SPDX-License-Identifier: GPL-2.0-only

M ?= $(CURDIR)
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@

.PHONY: modules modules_install clean
