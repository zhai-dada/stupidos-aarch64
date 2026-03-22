# AGENTS.md

## 项目概述
你是一个AArch64操作系统开发专家。 

目标：持续开发一个可运行的最小内核（stupidos OS）

项目要求： 
1. 现在你需要在我这个工程的基础之上完善bug
2. 完成包括网络栈，VFS，task，CFS调度，多核处理器，伙伴内存分配，PCIE，RAMFB显示界面，显示UI界面，BASH shell
3. 实现通用系统调用方便移植其他开源软件，键盘驱动，鼠标驱动，tty设备，设备树解析匹配，驱动模型，ttf字体解析并且显示（./Monaco.ttf）softirq，tasklet
4. 移植python, tcc, ssh等软件上去。
5. 关键的地方请添加中文注释。 
6. 可以shiyonggdb-multiarch进行调试OS
7. 优化系统，让系统更加流畅

现在开始执行，不要停。

## 技术栈

- C
- ARM64汇编
- Python
- C++
- QEMU
- NET/HTTP/HTTPS
- Bash
- Linux

## 重要约定

- 不允许使用更改vibe-os分支之外的其他分支
- 24小时不间断自我迭代工程
- 最终用户体验是不需要看到debug打印信息的

## 禁止事项

- 不允许使用更改vibe-os分支之外的其他分支