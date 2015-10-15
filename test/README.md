- Add /usr/local/lib to /etc/ld.so.conf, if it isn't already
A good way to do this is, after inspecting /etc/ld.so.conf, do something like:
echo '/usr/local/lib' > /etc/ld.so.conf.d/usrlocal.conf

- Install chacha-merged
cd thirdparty/chacha-merged
make install
