# Pinnacle 500-USB open driver — build.
#
# Builds a small static core library (device init, streaming, DV
# reassembly) and links it into the pincli capture tool. Kept as a separate
# library on purpose: a future GUI front-end links the same core.

CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0) -lpthread

CORE_SRCS := core/pinnacle_device.c core/pinnacle_1394.c core/pinnacle_stream.c core/dv_reassembler.c
CORE_OBJS := $(CORE_SRCS:.c=.o)

BUILD_DIR := ../build

.PHONY: all clean

all: $(BUILD_DIR)/pincli $(BUILD_DIR)/pindeck

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

core/%.o: core/%.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/libpinnacle500.a: $(CORE_OBJS) | $(BUILD_DIR)
	ar rcs $@ $(CORE_OBJS)

$(BUILD_DIR)/pincli: cli/pincli.c $(BUILD_DIR)/libpinnacle500.a | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -Icore -o $@ cli/pincli.c $(BUILD_DIR)/libpinnacle500.a $(LIBUSB_LIBS)

$(BUILD_DIR)/pindeck: cli/pindeck.c $(BUILD_DIR)/libpinnacle500.a | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -Icore -o $@ cli/pindeck.c $(BUILD_DIR)/libpinnacle500.a $(LIBUSB_LIBS)

clean:
	rm -f $(CORE_OBJS) $(BUILD_DIR)/libpinnacle500.a $(BUILD_DIR)/pincli $(BUILD_DIR)/pindeck
