ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := dpiv2.elf

CFLAGS := -Os -Wall -Werror

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^ -lkernel_sys -lSceNet
	$(STRIP) --strip-all $@

install: $(ELF)
	cp $(ELF) ../../payloads/dpiv2.elf

clean:
	rm -f $(ELF)
