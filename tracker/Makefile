CXX = g++
CXXFLAGS = -std=c++11 -pthread -Wall -Wextra

# OpenSSL (libssl, for TLS): use pkg-config where available (Linux),
# otherwise fall back to the Homebrew install (macOS).
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

TARGET = tracker
SRCS = server.cpp commands.cpp tls.cpp password.cpp
HDRS = commands.h tls.h password.h

# The certificates and keys live next to tracker_info.txt
CERT_DIR = ..

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(OPENSSL_CFLAGS) $(SRCS) $(OPENSSL_LIBS) -o $(TARGET)

# Creates the certificate authority (only if there is none yet), then a new
# key and a certificate signed by the authority for each tracker.
#   ca_cert.pem            for everybody: trackers and every client
#   ca_key.pem             signs tracker certificates; no program reads it,
#                          keep it away from the machines the trackers run on
#   tracker<n>_cert.pem    for tracker n
#   tracker<n>_key.pem     for tracker n only
# Running it again replaces the trackers' keys; the clients need nothing new.
cert:
	@test -f $(CERT_DIR)/ca_key.pem || openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
		-subj "/CN=p2p-ca" -keyout $(CERT_DIR)/ca_key.pem -out $(CERT_DIR)/ca_cert.pem
	@for n in 1 2; do \
		openssl req -newkey rsa:2048 -nodes -subj "/CN=p2p-tracker-$$n" \
			-keyout $(CERT_DIR)/tracker$${n}_key.pem -out $(CERT_DIR)/tracker$$n.csr && \
		openssl x509 -req -in $(CERT_DIR)/tracker$$n.csr -CA $(CERT_DIR)/ca_cert.pem \
			-CAkey $(CERT_DIR)/ca_key.pem -CAcreateserial -days 365 \
			-out $(CERT_DIR)/tracker$${n}_cert.pem && \
		rm -f $(CERT_DIR)/tracker$$n.csr || exit 1; \
	done
	@rm -f $(CERT_DIR)/ca_cert.srl
	@chmod 600 $(CERT_DIR)/ca_key.pem $(CERT_DIR)/tracker1_key.pem $(CERT_DIR)/tracker2_key.pem
	@echo "Created in $(CERT_DIR): ca_cert.pem ca_key.pem tracker1_cert.pem tracker1_key.pem tracker2_cert.pem tracker2_key.pem"

clean:
	rm -f $(TARGET)

.PHONY: all cert clean
