.PHONY: build upload test-host qemu clean

build:
	idf.py build

upload:
	idf.py flash monitor

# Tier 1 — host-native unit tests (DESIGN.md §12.2). No hardware, no emulator.
test-host:
	idf.py -C test/host --preview set-target linux build
	test/host/build/test_host.elf

# Tier 2 — QEMU integration tests (DESIGN.md §12.3).
qemu:
	idf.py qemu

clean:
	idf.py fullclean
