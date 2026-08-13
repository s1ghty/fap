CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -Wpedantic -Iinclude -Ivendor
LDFLAGS = -lcurl -lzstd -lcrypto

# Defaults to a per-user install; run via sudo (or as root) and it
# defaults to a system-wide install instead, matching fap's own
# dual-mode design (see CLAUDE.md's "Install root" bullet) — the
# fap binary itself ends up wherever fap-installed packages will
# look for their own bin dir. Override either way with
# `make install PREFIX=/some/other/path`.
ifeq ($(shell id -u),0)
PREFIX  ?= /usr/local
else
PREFIX  ?= $(HOME)/.local
endif
BINDIR  = $(PREFIX)/bin

SRC = src/main.c \
      src/cli.c \
      src/util.c \
      src/hash.c \
      src/config.c \
      src/json.c \
      src/lock.c \
      src/registry.c \
      src/resolver.c \
      src/package.c \
      src/install.c \
      vendor/toml.c

OBJ = $(SRC:.c=.o)
BIN = fap

.PHONY: all test install uninstall clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(BINDIR)
	install -m 755 $(BIN) $(BINDIR)/$(BIN)
ifneq ($(shell id -u),0)
	@case ":$$PATH:" in \
		*":$(BINDIR):"*) ;; \
		*) \
			rc=""; \
			case "$$SHELL" in \
				*/zsh)  rc="$$HOME/.zshrc" ;; \
				*/bash) rc="$$HOME/.bashrc" ;; \
			esac; \
			if [ -z "$$rc" ]; then \
				echo "note: $(BINDIR) is not on your PATH — add 'export PATH=\"$(BINDIR):\$$PATH\"' to your shell rc"; \
			elif [ -f "$$rc" ] && grep -qF '$(BINDIR)' "$$rc"; then \
				echo "note: $(BINDIR) is already referenced in $$rc — restart your shell or run: source $$rc"; \
			else \
				echo '' >> "$$rc"; \
				echo '# added by fap: make install' >> "$$rc"; \
				echo 'export PATH="$(BINDIR):$$PATH"' >> "$$rc"; \
				echo "note: added $(BINDIR) to PATH in $$rc — restart your shell or run: source $$rc"; \
			fi ;; \
	esac
endif

uninstall:
	rm -f $(BINDIR)/$(BIN)

test: tests/test_hash tests/test_lock tests/test_config tests/test_registry tests/test_resolver tests/test_package tests/test_install
	./tests/test_hash
	./tests/test_lock
	./tests/test_config
	./tests/test_registry
	./tests/test_resolver
	./tests/test_package
	./tests/test_install

tests/test_hash: tests/test_hash.c src/hash.c src/util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test_lock: tests/test_lock.c src/lock.c src/json.c src/util.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_config: tests/test_config.c src/config.c src/util.c vendor/toml.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_registry: tests/test_registry.c src/registry.c src/json.c src/util.c
	$(CC) $(CFLAGS) -o $@ $^ -lcurl

tests/test_resolver: tests/test_resolver.c src/resolver.c src/util.c
	$(CC) $(CFLAGS) -o $@ $^

tests/test_package: tests/test_package.c src/package.c src/util.c
	$(CC) $(CFLAGS) -o $@ $^ -lzstd -lcurl

tests/test_install: tests/test_install.c src/install.c src/package.c src/util.c
	$(CC) $(CFLAGS) -o $@ $^ -lzstd -lcurl

clean:
	rm -f $(OBJ) $(BIN) tests/test_hash tests/test_lock tests/test_config tests/test_registry tests/test_resolver tests/test_package tests/test_install
