PACKAGE := new-hidpp
PACKAGE_VERSION := $(shell git describe --long --match v6.*).$(shell date -u -d "$$(git show -s --format=%ci HEAD)" +%Y%m%d)

DKMS_ROOT_PATH := /usr/src/$(PACKAGE)-$(PACKAGE_VERSION)

ifndef TARGET
TARGET = $(shell uname -r)
endif

.PHONY: version dkms dkms_clean deb

version:
	echo $(PACKAGE_VERSION)

dkms:
	mkdir -p $(DKMS_ROOT_PATH)
	rsync --recursive --verbose --copy-links \
		--include '/*/' --include '*.c' --include '*.h' \
		--include 'Makefile' --include 'Kbuild' \
		--exclude '*' module/ $(DKMS_ROOT_PATH)/
	echo "$(PACKAGE_VERSION)" >$(DKMS_ROOT_PATH)/VERSION
	sed -e '/^PACKAGE_VERSION=/ s/=.*/=\"$(PACKAGE_VERSION)\"/' <dkms.conf >$(DKMS_ROOT_PATH)/dkms.conf
	dkms add -m $(PACKAGE) -v $(PACKAGE_VERSION)
	dkms build -m $(PACKAGE) -v $(PACKAGE_VERSION) -k $(TARGET)
	dkms install --force -m $(PACKAGE) -v $(PACKAGE_VERSION) -k $(TARGET)

dkms_clean:
	dkms remove -m $(PACKAGE) -v $(PACKAGE_VERSION) --all
	rm -rf $(DKMS_ROOT_PATH)

deb:
	if [ -d pkg ]; then rm -r pkg; fi
	mkdir pkg
	rsync --recursive --verbose --copy-links \
		--include '/*/' --include '*.c' --include '*.h' \
		--include 'Makefile' --include 'Kbuild' \
		--exclude '*' module/ pkg/module/
	rsync --recursive \
		--include 'source/' --include 'source/format' --include 'source/options' \
		--include 'changelog' --include 'control' --include 'dkms' --include 'rules' \
		--exclude '*' debian/ pkg/debian/
	cp dkms.conf pkg/
	cd pkg && debuild -uc -us
	rm -r pkg
