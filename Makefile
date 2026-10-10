PS5_HOST ?= ps5
PS5_PORT ?= 9021
MANAGER_URL ?= http://$(PS5_HOST):8084

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := ps5-date-time-sync.elf
CXXFLAGS := -Oz -Wall -Wextra -Werror -std=c++17 -fno-exceptions -fno-rtti
LDLIBS := -lSceNet -lSceNotification

.PHONY: all clean deploy test manager-deploy
.DELETE_ON_ERROR:

all: $(ELF)

$(ELF): main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(ELF)

# Sends the payload to an ELF loader.  `test` is kept as an alias because it
# is the PS5 Payload SDK's naming convention.
deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

test: deploy

manager-deploy: $(ELF)
	@echo "[PS5 Date/Time Sync] Uploading $(ELF) to $(MANAGER_URL)..."
	curl --fail --silent --show-error --data-binary "@$<" \
		"$(MANAGER_URL)/manage:upload?filename=$(ELF)"
	@echo
	@echo "[PS5 Date/Time Sync] Starting $(ELF) on the PS5..."
	curl --fail --silent --show-error \
		"$(MANAGER_URL)/loadpayload:$(ELF)"
	@echo
	@echo "[PS5 Date/Time Sync] Sync requested. Check the PS5 notification center or payload output."
