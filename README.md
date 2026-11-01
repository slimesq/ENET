# ENET

ECloudAssistant 的 Linux 服务端源码，迁移自 `GOICDZDYLMRXH9QB_20250515144010A008.rar` 中的 `ENET/`。归档自带的旧 `build/` 产物没有迁入。

| 模块 | 可执行文件 | 职责 | 源码中的监听端口 |
| --- | --- | --- | --- |
| `LoadBanceServer` | `LoadBanceSvr` | 选择登录服务器 | 8523 |
| `LoginServer` | `LoginSvr` | MySQL 用户查询，返回信令服务器地址 | 9867 |
| `SigServer` | `SigSvr` | 识别码注册、建流协调和输入事件转发 | 6539 |
| `RtmpServer` | `RtmpSvr` | RTMP 发布、播放与流会话分发 | 1935 |
| `EdoyunNet` | 静态库 | Linux `epoll`、TCP 连接、定时器和缓冲区 | — |

`TCPServer/` 与 `EdoyunNet/main.cpp` 是演示代码，不参与顶层构建。

## 构建

在 Linux 环境中准备 CMake、Ninja、支持 C++17 的编译器、线程库，以及 MySQL 或 MariaDB 客户端开发文件，然后运行：

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
```

Release 使用 `linux-release` 预设。没有 Ninja 时可改用 `linux-make-debug` 预设和系统的 Make。可执行文件生成于所选构建目录的 `bin/`，`compile_commands.json` 生成于对应构建目录。VS Code 配置可把编译数据库复制到工作区根目录供 clangd 使用。

若只需先编译不依赖数据库的三个服务，可在配置时使用 `-DENET_BUILD_LOGIN_SERVER=OFF`，例如 `cmake --preset linux-make-debug -DENET_BUILD_LOGIN_SERVER=OFF`。完整登录链仍需要 `LoginSvr`。

`LoginSvr` 启动前需要设置 `ENET_DB_PASSWORD`；源码仍以本机 MySQL、`root` 用户和 `users` 数据库为默认连接目标。数据库表结构和运行数据没有包含在归档中。

`LoadBanceSvr` 只监听 8523；9867 由独立的 `LoginSvr` 监听。负载服务依赖登录服务上报监控信息，尚未完成实际运行与协议联调。

## 已验证范围

迁移后已在 Ubuntu 22.04、CMake 3.22、G++ 11.4 上编译通过 `EdoyunNet`、`LoadBanceSvr`、`LoginSvr`、`SigSvr` 和 `RtmpSvr`。构建验证所用的 MariaDB 开发文件仅解包在临时目录，没有改动主机的系统安装。尚未启动服务，也没有验证数据库表结构、协议兼容或客户端联调。

## 与客户端联调前

服务端入口与内部连接地址目前写为 `172.21.205.121`，ECloudAssistant 客户端中的负载及 RTMP 地址写为 `192.168.31.30`。应先统一地址、核对两端协议结构体，再验证四个服务的启动顺序和运行结果。编译成功本身不代表整套远程控制链已经通过联调。

客户端学习路线及服务端对应阶段见 [`ECloudAssistant/docs/项目学习规划.md`](../ECloudAssistant/docs/项目学习规划.md)。
