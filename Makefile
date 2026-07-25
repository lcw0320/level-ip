CPPFLAGS = -I include -Wall -Werror -pthread

src = $(wildcard src/*.c)
obj = $(patsubst src/%.c, build/%.o, $(src))
headers = $(wildcard include/*.h)
apps = apps/curl/curl

lvl-ip: $(obj)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(obj) -o lvl-ip
	@echo
	@echo "lvl-ip needs CAP_NET_ADMIN:"
	sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip

build/%.o: src/%.c ${headers}
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# debug: CFLAGS+= -DDEBUG_SOCKET -DDEBUG_TCP -DDEBUG_UDP -DDEBUG_IP -DDEBUG_IPV6 -DDEBUG_ICMPV6 -DDEBUG_NDP -DDEBUG_ADDRCONF -g
debug: CFLAGS+= -DDEBUG_IPV6 -DDEBUG_ICMPV6 -DDEBUG_NDP -DDEBUG_ADDRCONF -g
debug: lvl-ip

apps: $(apps)
	$(MAKE) -C tools
	$(MAKE) -C apps/curl
	$(MAKE) -C apps/curl-poll
	$(MAKE) -C apps/curl-v6
	$(MAKE) -C apps/tcp-v6-server

all: lvl-ip apps

test: debug apps
	@echo
	sudo bash tests/test-run.sh

run: debug
	@echo
	sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip
	sudo ./lvl-ip

clean:
	rm build/*.o lvl-ip
