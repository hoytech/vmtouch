# System tuning parameters for vmtouch

## Limits on memory locking

See your operating system's `mlock(2)` manual page since it probably describes the limitations on this system call which vmtouch uses.

**NOTE** vmtouch keeps file descriptors open for each file it maps which can result in hitting the `RLIMIT_NOFILE` limit, but this will be fixed when 3111cb1a51a0a556dd86e4a7d4653e6198eb6a95 is merged (it cleans up the mapping logic so the descriptors can be closed but the memory mappings preserved).

### Linux

* **locked memory rlimit**: Processes typically have a limit on the number of memory that is locked. This can be raised with `ulimit -l` (see `RLIMIT_MEMLOCK` in [setrlimit(2)](http://linux.die.net/man/2/setrlimit)) if you are the super-user. Processes with the `CAP_IPC_LOCK` are not affected by this limit, and it can be raised for unprivileged processes by editing [limits.conf](http://linux.die.net/man/5/limits.conf).
* **vm.max_map_count**: This is a [sysctl](http://linux.die.net/man/8/sysctl) that controls the maximum number of VMAs (virtual memory areas) that a process can map. Since vmtouch needs a VMA for every file, this limits the number of files that can be locked by an individual vmtouch process.
* Since Linux 2.6.9 (?) there is no system-wide limit on the amount of locked memory.

### FreeBSD

* See [mlock(2)](https://www.freebsd.org/cgi/man.cgi?query=mlock&sektion=2&manpath=freebsd-release-ports)
* **sysctls**: see `vm.max_wired` and `vm.stats.vm.v_wire_count`
* **security.bsd.unprivileged_mlock**: Whether unprivileged users can lock memory

### OpenBSD

* Has both a per-process resource limit and a system-wide limit on locked memory, see [mlock(2)](http://man.openbsd.org/mlock.2)
