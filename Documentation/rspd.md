/etc/radvd.conf
interface tap0 {
    AdvSendAdvert on;
    AdvManagedFlag off;
    AdvOtherConfigFlag off;
    prefix fd00:1234:5678::/64 {
        AdvOnLink on;
        AdvAutonomous on;
    };
};

# 开启全局转发
sudo sysctl -w net.ipv6.conf.all.forwarding=1

# 关键！强制 enp4s0 继续接收 RA（2 = 即使开启转发也接收）
sudo sysctl -w net.ipv6.conf.enp4s0.accept_ra=2

# tap0 不需要接收 RA（它由你的协议栈处理）
sudo sysctl -w net.ipv6.conf.tap0.accept_ra=0

# 使用 ip6tables NPT（需要内核支持，大多数现代发行版已内置）
# 出站：将 tap0 侧的源地址转换为 enp4s0 的前缀
sudo ip6tables -t nat -A POSTROUTING -o enp4s0 -j MASQUERADE

sudo ip6tables -A FORWARD -i tap0 -o enp4s0 -j ACCEPT
sudo ip6tables -A FORWARD -i enp4s0 -o tap0 -j ACCEPT

# 如果 MASQUERADE 不支持 IPv6，用 NPT 手动映射：
# sudo ip6tables -t nat -A POSTROUTING -s <tap0侧前缀>::/64 -o enp4s0 \
#     -j NETMAP --to 240e:464:2192:3466::/64

sudo ip addr add fd00:1234:5678::1/64 dev tap0

sudo systemctl start radvd
sudo journalctl -u radvd --no-pager