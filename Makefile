
#
# sudo apt install libmosquitto-dev libjson-c-dev libsnmp-dev
#

CC=gcc
CFLAGS_INCLUDE=
CFLAGS_OPT_COMMON=-Wall -Wextra -Wpedantic -O3 -fstack-protector-strong
CFLAGS_OPT_STRICT=\
    -Wstrict-prototypes -Wold-style-definition \
    -Wno-cast-align -Wcast-qual -Wconversion \
    -Wfloat-equal -Wformat=2 -Wformat-security \
    -Winit-self -Wjump-misses-init -Wlogical-op -Wmissing-include-dirs \
    -Wnested-externs -Wpointer-arith -Wredundant-decls -Wshadow \
    -Wstrict-overflow=2 -Wswitch-default \
    -Wswitch-enum -Wundef -Wunreachable-code -Wunused \
    -Wwrite-strings -Wno-stringop-truncation
CFLAGS=$(CFLAGS_INCLUDE) $(CFLAGS_OPT_COMMON) $(CFLAGS_OPT_STRICT)
LDFLAGS=-lmosquitto -ljson-c -lnetsnmp
LDFLAGS_DISCOVER=-lnetsnmp
SOURCES=include/mqtt_linux.h include/util_linux.h include/config_linux.h include/snmp_linux.h
TARGET = trafmon
DISCOVER = discover
HOSTNAME = $(shell hostname)

##

$(TARGET): $(TARGET).c $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c $(LDFLAGS)
$(DISCOVER): $(DISCOVER).c include/snmp_linux.h
	$(CC) $(CFLAGS) -o $(DISCOVER) $(DISCOVER).c $(LDFLAGS_DISCOVER)
all: $(TARGET) $(DISCOVER)
clean:
	rm -f $(TARGET) $(DISCOVER)
format:
	clang-format -i $(TARGET).c $(DISCOVER).c include/*.h
test: $(TARGET)
	./$(TARGET) --config $(TARGET).$(HOSTNAME).default
.PHONY: all clean format test lint

##

DEFAULT_DIR = /etc/default
TARGET_DIR = /usr/local/bin
SYSTEMD_DIR = /etc/systemd/system
define stop_systemd_service
	-systemctl stop $(1) 2>/dev/null || true
endef
define install_systemd_service
	-systemctl disable $(2) 2>/dev/null || true
	cp $(1).service $(SYSTEMD_DIR)/$(2).service
	systemctl daemon-reload
	systemctl enable $(2)
	systemctl start $(2) || echo "Warning: Failed to start $(2)"
endef
install_systemd_service: $(TARGET).service
	$(call install_systemd_service,$(TARGET),$(TARGET))
install_default: $(TARGET).$(HOSTNAME).default
	cp $(TARGET).$(HOSTNAME).default $(DEFAULT_DIR)/$(TARGET)
install_target: $(TARGET)
	$(call stop_systemd_service,$(TARGET))
	cp $(TARGET) $(TARGET_DIR)/$(TARGET)
install: install_target install_default install_systemd_service
restart:
	systemctl restart $(TARGET)
.PHONY: install install_target install_default install_systemd_service restart

