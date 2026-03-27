#ifndef __EXT4_H__
#define __EXT4_H__

/*
 * 当前 ext4 驱动的范围：
 * 1. 读取 GPT 第 2 分区并挂载为根文件系统
 * 2. 支持 extent 格式的目录遍历和普通文件读写
 * 3. 支持最小 create/mkdir/unlink/rename（无 journal）
 * 4. 写路径支持“缺块时分配数据块 + inode bitmap/block bitmap 更新”
 */
int ext4_mount_root(void);

#endif
