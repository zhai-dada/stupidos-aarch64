#ifndef STUPIDOS_NUM_APPLETS_H
#define STUPIDOS_NUM_APPLETS_H

/*
 * BusyBox lineedit / ash 在最小接入模式下仍会包含这个头。
 * 当前 stupidos 不是 BusyBox 多 applet 统一二进制布局，
 * 这里给一个稳定的最小值即可。
 */
#define NUM_APPLETS 1

#endif
