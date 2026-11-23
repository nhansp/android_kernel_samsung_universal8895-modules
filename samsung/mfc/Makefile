# SPDX-License-Identifier: GPL-2.0-only
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(CURDIR)

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@
