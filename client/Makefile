CXX = g++
CXXFLAGS = -std=c++11 -pthread -Wall -Wextra

# OpenSSL (libcrypto for SHA-1, libssl for TLS): use pkg-config where
# available (Linux), otherwise fall back to the Homebrew install (macOS).
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null)
ifeq ($(OPENSSL_LIBS),)
OPENSSL_DIR := $(shell brew --prefix openssl@3 2>/dev/null)
ifneq ($(OPENSSL_DIR),)
OPENSSL_CFLAGS := -I$(OPENSSL_DIR)/include
OPENSSL_LIBS := -L$(OPENSSL_DIR)/lib -lssl -lcrypto
else
OPENSSL_LIBS := -lssl -lcrypto
endif
endif

TARGET = client
SRCS = client.cpp peer.cpp fileutils.cpp threadpool.cpp tls.cpp
HDRS = peer.h fileutils.h threadpool.h tls.h console.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(OPENSSL_CFLAGS) $(SRCS) $(OPENSSL_LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
