
Follow everything documented here: https://github.com/chipsalliance/dromajo/blob/master/doc/setup.md#linux-with-buildroot
(get linux, openSBI, etc)

Helpful hints (as documented): include `buildroot-2020.05.1/output/host/bin` in your path. Set `CROSS_COMPILE` to use `riscv64-linux-`.

Follow the directions here: https://github.com/chipsalliance/dromajo/blob/master/doc/simpoint.md#select-the-benchmark-to-run
You do not need SimPoint unless you want to use it.

You can use the compiler that was built/installed with the buildroot