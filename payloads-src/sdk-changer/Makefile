ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := sdk-changer.elf
CFLAGS := -Os -Wall -Iinclude -Isrc

all: $(ELF)

CFILES := src/main.c src/elf2fself.c src/sha256.c

$(ELF): $(CFILES)
	$(CC) $(CFLAGS) -o $@ $^ -lkernel_sys
	$(STRIP) --strip-all $@

install: $(ELF)
	cp $(ELF) ../../payloads/sdk-changer.elf

clean:
	rm -f $(ELF)
