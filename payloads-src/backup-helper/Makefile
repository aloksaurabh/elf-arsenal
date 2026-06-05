ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := backup-helper.elf

CFLAGS := -Os -Wall -Werror -g

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^ -lkernel_sys
	$(STRIP) --strip-all $@

install: $(ELF)
	cp $(ELF) ../../payloads/backup-helper.elf

clean:
	rm -f $(ELF)
