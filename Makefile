SITE_BUILD_DIR ?= build/site
SITE_BOOT_IMAGE ?= boot/mini.raw
SITE_WEB_IMAGE ?= $(SITE_BUILD_DIR)/assets/taijios-web.raw

.PHONY: site clean-site serve-site driver-smoke kryon-native-plan9-smoke debian-smoke linux-support-smoke linuxrun-smoke unified-smoke window-smoke desktop-boot-smoke pcvirt

site:
	rm -rf "$(SITE_BUILD_DIR)"
	mkdir -p "$(SITE_BUILD_DIR)"
	cp -R site/. "$(SITE_BUILD_DIR)/"
	mkdir -p "$(SITE_BUILD_DIR)/assets"
	python3 scripts/make-web-image.py "$(SITE_BOOT_IMAGE)" site/assets/taijios-web-plan9.ini "$(SITE_WEB_IMAGE)"
	test -f "$(SITE_BUILD_DIR)/index.html"
	test -f "$(SITE_BUILD_DIR)/favicon.ico"
	test -f "$(SITE_BUILD_DIR)/css/style.css"
	test -f "$(SITE_BUILD_DIR)/js/taijios-web.js"
	test -f "$(SITE_BUILD_DIR)/assets/taijios-banner.png"
	test -f "$(SITE_BUILD_DIR)/assets/taijios-desktop.png"
	test -f "$(SITE_WEB_IMAGE)"
	test -f "$(SITE_BUILD_DIR)/assets/v86/libv86.js"
	test -f "$(SITE_BUILD_DIR)/assets/v86/v86.wasm"
	test -f "$(SITE_BUILD_DIR)/assets/v86/seabios.bin"
	test -f "$(SITE_BUILD_DIR)/assets/v86/vgabios.bin"

clean-site:
	rm -rf "$(SITE_BUILD_DIR)"

serve-site: site
	cd "$(SITE_BUILD_DIR)" && python3 -m http.server 8000

driver-smoke:
	sh scripts/driver-smoke.sh

debian-smoke:
	sh scripts/taiji-support.sh debian-smoke

linux-support-smoke:
	sh scripts/taiji-support.sh linux-support-suite

linuxrun-smoke:
	sh scripts/taiji-support.sh linuxrun-smoke

unified-smoke:
	sh scripts/taiji-support.sh unified-smoke

window-smoke:
	sh scripts/taiji-support.sh window-smoke

desktop-boot-smoke:
	sh scripts/desktop-boot-smoke.sh

pcvirt:
	sh scripts/taiji-support.sh build-pcvirt

kryon-native-plan9-smoke:
		sh scripts/kryon-native-plan9-smoke.sh

kryon-smoke:
	sh scripts/kryon-smoke.sh

.PHONY: rill-ziran-plan9-smoke
rill-ziran-plan9-smoke:
	sh scripts/rill-ziran-plan9-smoke.sh

.PHONY: rio-ziran-plan9
rio-ziran-plan9:
	env -u DISPLAY -u WAYLAND_DISPLAY ../../ziranlang/ziran/build/bin/ziran build --target=plan9-c --root sys/src/cmd/rio9 --module-path ../../ziranlang/ziran/std -o sys/src/cmd/rio9/build/ziran/plan9 sys/src/cmd/rio9/window_snapshot.zi
