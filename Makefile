CPPFLAGS = -I include -Wall -Werror -pthread

src = $(wildcard src/*.c)
obj = $(patsubst src/%.c, build/%.o, $(src))
headers = $(wildcard include/*.h)

# 自动收集 apps 和 tools 下带 Makefile 的子目录
SUBDIRS = $(shell find tools apps -mindepth 2 -maxdepth 2 -name Makefile | xargs -n1 dirname)

lvl-ip: $(obj)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(obj) -o lvl-ip
	@echo
	@echo "lvl-ip needs CAP_NET_ADMIN:"
	sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip

build/%.o: src/%.c ${headers}
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

debug: CFLAGS+= -DDEBUG_SOCKET -DDEBUG_TCP -DDEBUG_UDP -DDEBUG_IP -DDEBUG_IPV6 -DDEBUG_ICMPV6 -DDEBUG_NDP -DDEBUG_ADDRCONF -fsanitize=address -g
debug: lvl-ip

.PHONY: apps
apps:
	@for dir in $(SUBDIRS); do \
	    echo "== building $$dir =="; \
	    $(MAKE) -C $$dir || exit 1; \
	done

all: lvl-ip apps

test: debug apps
	@echo
	sudo bash tests/test-run.sh

run: debug
	@echo
	sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip
	sudo ./lvl-ip

clean:
	rm -f build/*.o lvl-ip
	@for dir in $(SUBDIRS); do \
	    if $(MAKE) -C $$dir clean >/dev/null 2>&1; then :; \
	    else rm -f $$dir/$$(basename $$dir); fi; \
	done
