// ioctl: no device the host opens takes one on Windows (the Android port's KGSL clock floor, gfx_vk_floor.inc, finds no descriptor): every call fails with ENOTTY
#pragma once
#include "dm_windows.h"
int ioctl(int fd, unsigned long req, ...);
#define FIONREAD 0x541B
#define FIONBIO 0x5421
#define _IOC(dir, type, nr, size) (((dir) << 30) | ((type) << 8) | (nr) | ((unsigned long)(size) << 16))
#define _IO(t, n) _IOC(0, (t), (n), 0)
#define _IOW(t, n, s) _IOC(1, (t), (n), sizeof(s))
#define _IOR(t, n, s) _IOC(2, (t), (n), sizeof(s))
#define _IOWR(t, n, s) _IOC(3, (t), (n), sizeof(s))
