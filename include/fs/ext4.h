#ifndef __EXT4_H__
#define __EXT4_H__

/*
 * 当前 ext4 驱动的范围：
 * 1. 读取 GPT 第 2 分区并挂载为根文件系统
 * 2. 支持 extent 格式的目录遍历和普通文件读写
 * 3. 写入仅覆盖已有数据块，不做新块分配、truncate 和 journal
 */
int ext4_mount_root(void);

#endif
