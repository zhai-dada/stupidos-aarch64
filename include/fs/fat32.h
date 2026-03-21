#ifndef __FAT32_H__
#define __FAT32_H__

#include "asm/types.h"

/*
 * 当前 FAT32 驱动的能力边界：
 * 1. 从 GPT 第 1 分区挂载 FAT32
 * 2. 支持短文件名目录遍历
 * 3. 支持普通文件读取
 * 4. 支持对已有 cluster 链的覆盖式写入，并同步更新文件大小
 *
 * 当前明确不支持：
 * - 长文件名 LFN
 * - 新 cluster 分配
 * - create / unlink / rename
 */
int fat32_mount(const int8_t *path);

#endif
