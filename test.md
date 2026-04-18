yes "Hello Network World, this is a test line." | head -c 1048576000 > 1000MB.txt

sudo valgrind --leak-check=full --show-leak-kinds=all --log-file=valgrind.log ./lvl-ip
dd if=/dev/zero bs=1M count=5 | nc 10.0.0.4 8081