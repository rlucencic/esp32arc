# Host build of the Archimedes core: make -C components/archimedes/host
#   build/arc_host <rom> [frames] [out.ppm] [ram_kb]   (see host_main.c)
ARM2      := ../../../../esp32emu/components/arm2
EMUDBG    := ../../../../esp32emu/components/emudbg
ARC       := ..
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DARM2_HOST_HOOK \
             -I$(ARC)/include -I$(ARC) -I$(ARM2)/include -I$(EMUDBG)/include

ARC_SRCS  := $(wildcard $(ARC)/*.c)
ARM2_SRCS := $(ARM2)/arm2.c $(ARM2)/arm2_dis.c $(ARM2)/arm2_dbg.c
ARC_OBJS  := $(patsubst $(ARC)/%.c,$(BUILD)/arc/%.o,$(ARC_SRCS))
ARM2_OBJS := $(patsubst $(ARM2)/%.c,$(BUILD)/arm2/%.o,$(ARM2_SRCS)) \
             $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o

$(BUILD)/arc_host: host_main.c $(ARC_OBJS) $(ARM2_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/arc/%.o: $(ARC)/%.c $(wildcard $(ARC)/*.h $(ARC)/include/*.h) $(ARM2)/include/arm2.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arm2/%.o: $(ARM2)/%.c $(ARM2)/include/arm2.h $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: clean
