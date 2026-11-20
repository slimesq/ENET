# ENET 阶段二：TCP 字节流与自定义协议学习讲义

> 对应[项目学习规划的阶段二](项目学习规划.md#stage-2)。参考用时：4～6 小时，基础补学、环境配置和排障另计。
>
> 复用已有 TCP 和 C++ 知识，围绕一条 `JOIN` 请求，解释字段怎样变成字节、分次收到的字节怎样组成消息，以及处理后哪些字节应被保留。本阶段以源码和字节手推为主。

## 1. 学习顺序与范围

按以下顺序学习：

1. 对照协议文件，认清包头、长度是否包含包头，以及各命令对应的服务和消息方向。
2. 展开一条 `JOIN` 的 14 个字节；布局或字节序有疑问时，运行本讲的小探针。
3. 认识 `BufferReader` 的可读范围，手推分片、连续两包和完整包后的半包。
4. 回到收包源码，区分应有行为与当前实现，再核对客户端对应字段。

主要源码：

| 位置 | 本阶段看什么 |
| --- | --- |
| [LoginServer/define.h](../LoginServer/define.h) | `packet_head`、`UserLogin`、`LoginResult` |
| [LoadBanceServer/loaddefine.h](../LoadBanceServer/loaddefine.h) | `Login_Info`、`LoginReply`；负载协议复用登录协议的包头与命令 |
| [SigServer/define.h](../SigServer/define.h) | 独立定义的包头、`JOIN` 及其他信令包 |
| [BufferReader.h](../EdoyunNet/BufferReader.h) | 可读字节、查看与消费接口 |
| [SigConnection.cpp](../SigServer/SigConnection.cpp) | `OnRead → HandleMessage → HandleJion` 及消费位置 |
| [客户端 defin.h](https://github.com/slimesq/ECloudAssistant/blob/main/UI/tool/defin.h) | 对应字段、命令值与排列 |

当前先把收包回调的输入看作一段已有字节，业务处理看作分发后的接口。事件调度在阶段三、完整读写路径在阶段四、信令状态在阶段七展开。

## 2. 从包头确定消息边界

### 2.1 TCP 提供字节流，项目自己定义消息

先后发送消息 `P`、`Q` 后，接收端可能需要多次读取才能收齐，也可能一次就读到 `P + Q`。应用层消息边界需要由协议字段恢复；TCP 不保留应用的发送边界，见 [tcp(7)](https://man7.org/linux/man-pages/man7/tcp.7.html)。本讲中的“包”和“帧”均指一条自定义协议消息，“半包”指尚未收齐的消息。

本项目使用的包头字段为：

| 字段 | 类型 | 当前用途 |
| --- | --- | --- |
| `len` | `uint16_t` | 整条消息的字节数，包含包头 |
| `cmd` | `uint16_t` | 命令编号，供对应服务分发 |

当前布局中，包头共 4 字节。消息构造函数通常设置 `len = sizeof(具体消息类型)`，因此收齐条件是“可读字节数至少达到 `len`”，而不是“包头长度再加 `len`”。

`len` 的可表达上限是 65535，包含包头；缓冲区再大，也不会扩大这个字段的表达范围。`BufferReader::Read()` 一次最多读取 4096 字节，这个限制针对单次读取量，不是整条消息的长度上限。

### 2.2 命令编号不能单独决定包长

下表列出当前构建环境中，使用 GCC 默认枚举选项和仓库打包设置时的消息大小：

| 服务与方向 | 类型 | `cmd` | 整包字节数 |
| --- | --- | --- | --- |
| 客户端 → 负载服务，查询登录入口 | `Login_Info` | `Login = 2` | 12 |
| 负载服务 → 客户端 | `LoginReply` | `Login = 2`，部分错误分支改为 `ERROR` | 22 |
| 客户端 → 登录服务 | `UserLogin` | `Login = 2` | 77 |
| 登录服务 → 客户端 | `LoginResult` | `Login = 2` | 26 |
| 客户端 → 信令服务，登记识别码 | `Join_body` | `JOIN = 5` | 14 |
| 信令服务 → 客户端，登记应答 | `JoinReply_body` | `JOIN = 5` | 8 |

因此，确定具体消息还要结合服务、方向和所处状态。`JOIN` 请求与应答共用命令编号，不代表包体相同。

`Cmd` 指定了 `uint16_t` 底层类型；`ResultCode` 没有指定底层类型，在当前构建环境中占 4 字节。`pack(1)` 不会把所有枚举都变成 1 字节，表中的大小也不保证适用于其他编译器和编译选项。

## 3. 展开一条 JOIN 请求

### 3.1 字段与字节

在 [信令协议](../SigServer/define.h)中，`Join_body` 继承包头，再增加 `std::array<char, 10> id`。构造函数设置 `cmd = JOIN`、`len = sizeof(Join_body)`，并把 `id` 清零。

对新构造的对象调用 `SetId("345")`，当前小端布局如下：

| 相对消息起点的偏移 | 字段 | 字节数 | 示例内容 |
| --- | --- | --- | --- |
| 0～1 | `len` | 2 | `0E 00`，十进制 14 |
| 2～3 | `cmd` | 2 | `05 00`，十进制 5 |
| 4～13 | `id` | 10 | `33 34 35`，随后 7 个 `00` |

```text
0E 00 | 05 00 | 33 34 35 00 00 00 00 00 00 00
 len  | JOIN  |             id[10]
```

`33 34 35` 分别是字符 `'3'`、`'4'`、`'5'` 的编码，并非整数 345 的二进制表示。识别码只有 3 个字符，数组仍占 10 字节，整包长度仍是 14。

### 3.2 打包与字节序分别解决什么

协议头文件用 `#pragma pack(push, 1)` 限制成员对齐，并在末尾用 `pop` 恢复。它影响当前编译器的对象布局，见 [GCC 的结构布局说明](https://gcc.gnu.org/onlinedocs/gcc/Structure-Layout-Pragmas.html)。

两端发送自定义包时直接使用对象内存，包头和包体字段没有逐字段转换为网络大端。在当前 x86/x64 小端环境中，数值 14 保存为 `0E 00`。设置 socket 地址时对端口调用 `htons()`，不会同时转换消息字段。

`pack(1)` 不负责字节序、固定字符串终止、命令合法性或收包长度检查。把网络字节直接强制转换成结构体指针，也是当前实现对布局和对象访问方式的依赖，不能据此把原始内存发送当作通用序列化方案。RTMP 的编码规则在阶段八另学。

### 3.3 固定数组与字符串终止

`Join_body` 构造时清零数组，所以新对象写入 `"345"` 后，剩余位置仍是零。但 `SetId()` 中的 `std::string::copy` 本身不会追加 `'\0'`，见 [copy 的说明](https://learn.microsoft.com/en-us/cpp/standard-library/basic-string-class?view=msvc-170#copy)。

`GetId()` 用 `std::string(id.data())` 按 C 字符串读取，要求数组内包含 `'\0'` 终止符。填满 10 个非零字节时，就没有终止符；按这种读取方式，应为终止符保留位置。复用对象改写短字符串时，也要检查旧内容是否已清除。

因此，“收到 14 字节”只证明这一帧按声明长度到齐，不能证明识别码内容已经合法。

### 3.4 按需运行布局探针

布局已经清楚时，直接进入第四节。有疑问时，在 **Linux／WSL 终端的仓库根目录**执行以下片段；只包含信令头文件，不需要启动服务。

```sh
probeDir="$(mktemp -d)"
c++ -std=c++17 -I. -x c++ - -o "$probeDir/EnetJoinLayout" <<'CPP'
#include "SigServer/define.h"
#include <cstddef>
#include <cstdio>

int main()
{
    Join_body request{};
    request.SetId("345");
    std::printf("head=%zu join=%zu len@=%zu cmd@=%zu\n",
        sizeof(packet_head), sizeof(request),
        offsetof(packet_head, len), offsetof(packet_head, cmd));
    std::printf("len=%u cmd=%u\n",
        static_cast<unsigned int>(request.len),
        static_cast<unsigned int>(request.cmd));
    const auto* bytes{reinterpret_cast<const unsigned char*>(&request)};
    for (std::size_t index{}; index < sizeof(request); ++index)
    {
        std::printf("%02X%s", static_cast<unsigned int>(bytes[index]),
            index + 1 == sizeof(request) ? "\n" : " ");
    }
}
CPP
"$probeDir/EnetJoinLayout"
```

在本讲的小端布局下，应看到：

```text
head=4 join=14 len@=0 cmd@=2
len=14 cmd=5
0E 00 05 00 33 34 35 00 00 00 00 00 00 00
```

`offsetof` 在这里仅用于包头。`Join_body` 的基类和派生类都有数据成员，不是 standard-layout 类型；对它使用 `offsetof(Join_body, id)` 的支持取决于实现，见 [C++ 对 offsetof 的约束](https://eel.is/c++draft/support.types.layout)。本例通过实际字节观察派生对象的布局。

探针先构造 `request`，再用 `unsigned char` 读取这个已有对象的字节表示。这是 C++ 允许的访问方式，见[对象的类型访问规则](https://eel.is/c++draft/basic.lval#11)。

接收端把字节缓冲强转为 `Join_body*` 则是另一件事：指针转换本身不会创建对象，也不会检查长度或字段，见 [WG21 对指针转换与对象创建的说明](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0593r6.html#when-to-create-objects)。因此，探针输出符合预期，不能证明接收端的结构体访问方式已经满足全部条件。

登录与信令头文件各自定义了同名的 `Cmd`、`ResultCode` 和 `packet_head`，应分别观察，直接放进同一源文件会产生重定义。

## 4. 从可读字节到完整消息

### 4.1 先认清 BufferReader 的三个动作

对照 [BufferReader.h](../EdoyunNet/BufferReader.h)：

| 接口或字段 | 含义 |
| --- | --- |
| `reader_index_` | 当前可读区起点 |
| `writer_index_` | 已收到数据的尾后位置，也是下一次追加数据的起点 |
| `ReadableBytes()` | `writer_index_ - reader_index_` |
| `Peek()` | 返回可读起点，不消费数据 |
| `Retrieve(n)` | 从可读区消费 `n` 字节；达到或超过当前可读量时，将两个索引归零 |

可读区是半开区间 `[reader_index_, writer_index_)`，`Peek()` 指向 `Begin() + reader_index_`。一次实际收到 `n` 字节后，writer 增加 `n`；例如 reader 为 14、writer 为 21 时，保留的是偏移 14～20 的 7 字节，再收到 7 字节后 writer 变为 28。

`Retrieve()` 改变的是可读范围，不是每次都搬动或擦除底层内存；`RetrieveAll()` 归零索引也不等于释放缓冲区。

`Retrieve()` 本身不验证协议长度，有数据时 `Retrieve(0)` 不会前进。调用者必须先确定该消费多少。

### 4.2 应有的解析顺序

先按以下顺序手推，再与第五节的当前代码比较：

```text
可读不足 4 字节
  → 保留全部字节，退出本轮解析，等待后续输入

包头已到齐
  → 检查 len 至少为 4，并在允许的消息长度范围内
  → 结合服务、方向和 cmd，核对该消息类型规定的整包长度；JOIN 请求应为 len == 14
  → 当前可读不足 len：保留字节并退出本轮解析
  → 整帧到齐：检查必须读取消息体才能判断的条件，例如 id 是否包含终止符
  → 分发完整合法消息
  → 消费这一帧 len 字节
  → 若仍有字节，继续检查下一帧
```

非法长度应进入明确的拒绝或关闭分支。没有可靠同步标记时，随意跳过一个字节重试，不能保证找回正确边界。本阶段只解释这些分支，不要求另写分包器。

### 4.3 一个包分三次收到

使用第三节的同一条 `JOIN`，从空缓冲开始，假定每次已把输入追加完毕：

| 本次输入 | 累计可读字节 | 应有处理 | 处理后 reader / writer |
| --- | --- | --- | --- |
| 前 2 字节：`0E 00` | 2 | 包头不足，不消费并返回 | 0 / 2 |
| 再 4 字节：`05 00 33 34` | 6 | 已知 `len=14`，还缺 8 字节，返回 | 0 / 6 |
| 再 8 字节：`35` 及 7 个 `00` | 14 | 识别一帧，消费 14 字节 | 0 / 0 |

前两次返回后，控制权交回事件循环，下一次读事件才能继续追加。当前服务端信令的 `OnRead()` 会在可读字节数不变时退出本轮解析，所以上表中未收齐的字节会留在缓冲区。

### 4.4 一次收到两条完整消息

令 `P`、`Q` 都是 14 字节的 `JOIN` 请求，一次读入 28 字节：

| 动作 | reader / writer | 剩余可读 |
| --- | --- | --- |
| 收到 `P + Q` | 0 / 28 | 28 |
| 处理 P，`Retrieve(14)` | 14 / 28 | 14 |
| 处理 Q，`Retrieve(14)` | 0 / 0 | 0 |

这里说明解析器能够识别两帧。同一连接第二次登记是否被接受，是业务状态问题；不能把两帧都被识别等同于两次登记成功。

### 4.5 完整包后跟半包

先收到 `P` 的全部 14 字节，加上 `Q` 的前 7 字节：

| 动作 | reader / writer | 剩余可读 |
| --- | --- | --- |
| 收到 21 字节 | 0 / 21 | 21 |
| 处理 P，消费 14 | 14 / 21 | 7 |
| Q 的包头已齐，但不足 14 字节，返回 | 14 / 21 | 7 |
| 再收到 Q 剩余 7 字节 | 14 / 28 | 14 |
| 处理 Q，消费 14 | 0 / 0 | 0 |

只消费完整包，保留未完成的尾部。缓冲区扩容、前缀回收和指针有效期，在阶段四继续检查。

## 5. 对照当前收包代码

### 5.1 信令怎样逐包处理并保留半包

先看 [SigConnection::OnRead](../SigServer/SigConnection.cpp)：

```cpp
while (buffer.ReadableBytes() > 0)
{
    auto const before{buffer.ReadableBytes()};
    this->HandleMessage(buffer);
    if (buffer.ReadableBytes() == before)
    {
        break;
    }
}
```

再看 `HandleMessage()`：可读字节数小于 `sizeof(packet_head)` 或 `data->len` 时，函数直接 `return`；完整消息分发后，才执行 `buffer.Retrieve(data->len)`。

外层用 `before` 判断本轮是否消费了字节。消费一条完整消息后，循环可以检查下一条。未消费时执行 `break`，`OnRead()` 随后返回 `true`，连接继续等待后续数据。

| 输入 | 当前信令实现的结果 |
| --- | --- |
| 两条完整合法帧 | 同一次回调中逐帧分发并消费 |
| 不足 4 字节的包头 | 保留已有字节，退出本轮解析 |
| 包头完整，但不足 `len` | 保留未收齐的消息，等下次读取 |
| 完整包后跟半包 | 消费完整包，保留尾部半包并退出本轮解析 |

### 5.2 登录和负载采用同样的解析循环

[LoginConnection::OnRead](../LoginServer/LoginConnection.cpp) 和 [LoadBanceConnection::OnRead](../LoadBanceServer/LoadBanceConnection.cpp) 都会在同一次回调中处理已有的完整请求，碰到尾部半包时退出并保留字节。这样，用户态缓冲区中的下一条完整请求无需等 socket 再次报告可读。

### 5.3 长度校验还缺什么

当前这些处理器主要检查“包头是否到齐”和“可读字节数是否达到声明的 `len`”，并没有完整检查声明长度及具体消息体。

只用字节和控制流回答下列问题即可：

| 情况 | 当前检查为什么不够 |
| --- | --- |
| `len=0` | 可通过现有检查并进入分发。如果处理函数返回，`Retrieve(0)` 不推进，外层退出本轮解析，非法帧仍留在缓冲区 |
| `0 < len < 4` | 声明长度连包头都容不下，却仍可能进入分发并错误移动边界 |
| `cmd=JOIN`、`len=4` | 只有包头却进入 `HandleJion`，按 `Join_body` 读取 id 会越过当前帧的有效范围 |
| id 的 10 字节全非零 | 帧长正确，`GetId()` 所需的字符串终止条件仍不满足 |

业务函数把指针转换成 `Join_body*`，并不会自动完成这些检查。对于当前固定格式的 JOIN 请求，应检查整包长度是否为 14 字节，并核对识别码字段。

## 6. 核对客户端对应字节

当前客户端 [SigConnection::Join](https://github.com/slimesq/ECloudAssistant/blob/main/Net/SigConnection.cpp) 构造 `Join_body` 后，从对象地址起发送 `body.len` 个字节；数据写入发送缓冲区时按原样复制。对照两端定义，可以确认 JOIN 请求的字段顺序、`cmd=5`、10 字节 id 及当前 14 字节布局一致。

其他消息按用到的范围继续补。以键盘和滚轮为例，客户端和服务端采用相同的包体布局。

| 消息 | 包头后的布局 |
| --- | --- |
| 键盘 | 先是 1 字节的操作类型 `type`，再是 2 字节的键值 `key` |
| 滚轮 | 1 字节的 `int8_t wheel`，可表示向上和向下 |

服务端 `HandleOtherMessage()` 原样转发输入包字节，由被控端解析包体。输入语义在客户端对应阶段学习，实际输入结果在统一联调时确认。

把本阶段已读到的 JOIN 命令、整包长度和收发位置补入[共同协议记录](项目学习规划.md#joint-validation)即可，其余包随使用补充。应区分：

- 布局相同：字段偏移、宽度和长度一致。
- 语义一致：字段取值在两端表示同一件事。
- 实际互通：真实收发、解析和业务结果已观察通过。

## 7. 自检与下一步

用同一条 JOIN 字节回答：

1. 为什么识别码只有 `"345"`，`len` 仍是 14？为什么不能再把包头加一次？
2. 先收到 2 字节、再追加 4 字节（累计 6 字节）时，为什么应保留数据并退出解析？
3. 一次到 28 字节，处理第一帧后 `Peek()` 应指向哪里？
4. `HandleMessage()` 在半包时直接返回，外层怎样知道该结束本轮解析？这一判断为什么不能代替包长校验？
5. 两端都用了 `pack(1)`，为什么还要检查字节序、字符串和字段含义？

完成标准：

- [ ] 能逐字段解释 14 字节 JOIN，并说明观察所依赖的当前布局与字节序。
- [ ] 能推演三组输入的可读量、消费位置和退出时机。
- [ ] 能在源码中指出消息完整性检查、分发和消费位置，区分当前缺陷与应有行为。

能解释这些问题后，进入[阶段三：epoll 与事件循环](项目学习规划.md#stage-3)，再看字节如何通过读事件进入这条解析路径。
