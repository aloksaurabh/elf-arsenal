ELF := klogsrv.elf

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK undefined; export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -Os -Wall

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $< -lkernel_sys
	$(STRIP) --strip-all $@

install: $(ELF)
	cp $(ELF) ../../payloads/klogsrv.elf

clean:
	rm -f $(ELF)

.PHONY: install clean
