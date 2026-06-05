ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := dpi.elf

CFLAGS := -Os -Wall -Werror

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^ -lkernel_sys -lSceAppInstUtil -lSceNet
	$(STRIP) --strip-all $@

install: $(ELF)
	cp $(ELF) ../../payloads/dpi.elf

clean:
	rm -f $(ELF)
