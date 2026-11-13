# ENET 阶段四：TCP 连接、收发缓冲区与背压学习讲义

> 对应[项目学习规划的阶段四](项目学习规划.md#stage-4)。参考用时：6～10 小时，基础补学、环境配置和排障另计。
>
> 沿一条消息追踪“读入字节 → 业务消费 → 准备应答 → 排队 → 分次写出”。复用已有 socket 知识，重点核对缓冲区索引、数据所有权、事件推进及错误反馈。

## 1. 学习顺序与必要概念

按以下顺序学习：

1. 认清连接对象持有的 Channel、读缓冲和写队列。
2. 跟踪 `TcpConnection::HandleRead → BufferReader::Read → readCb_`，核对读取、扩容和消费。
3. 比较两个 `TcpConnection::Send()` 重载，确认哪里复制、哪里共享存储。
4. 跟踪 `HandleWrite → BufferWriter::Send → ::send`，手推部分发送与写事件关注。
5. 用队列满、暂时不可读写和关闭分支，判断当前背压与错误反馈还缺什么。

主要源码：

| 位置 | 本阶段看什么 |
| --- | --- |
| [TcpServer.cpp](../EdoyunNet/TcpServer.cpp) | `OnConnect → AddConnection`，连接怎样保存到表中 |
| [TcpConnection.h](../EdoyunNet/TcpConnection.h)、[TcpConnection.cpp](../EdoyunNet/TcpConnection.cpp) | 缓冲所有者、读写入口、锁范围、关闭入口 |
| [BufferReader.h](../EdoyunNet/BufferReader.h)、[BufferReader.cpp](../EdoyunNet/BufferReader.cpp) | 可读区、实际读取、扩容、消费与 `ReadAll` |
| [BufferWriter.h](../EdoyunNet/BufferWriter.h)、[BufferWriter.cpp](../EdoyunNet/BufferWriter.cpp) | 队列项、两种入队方式、发送索引与返回值 |
| [SigConnection.cpp](../SigServer/SigConnection.cpp) | 用完整 JOIN 请求及其应答串起读写路径 |

### 1.1 先区分几个容易混淆的结果

| 结果 | 能说明什么 |
| --- | --- |
| 调用了 `TcpConnection::Send()` | 进入了业务发送接口；当前接口不返回本次入队结果 |
| `BufferWriter::Append()` 返回 true | 数据已被本地发送队列接受 |
| 底层 `::send()` 返回正数 n | 本机 socket 接受了 n 字节，不代表对端业务已经处理 |
| 本地发送队列为空 | 该队列中已没有待交给 socket 的字节 |
| 对端收到并按协议处理 | 需要对端接收、解析或业务应答等实际结果确认 |

`TcpConnection::Send()`、`BufferWriter::Send()` 和系统调用 `::send()` 分别是业务发送接口、队列发送方法和系统调用，它们的返回值与完成含义不能混用。系统调用语义见 [send(2)](https://man7.org/linux/man-pages/man2/send.2.html)。

### 1.2 返回值知识按需复核

在本项目的非阻塞 TCP socket 上，先按以下规则理解返回值。讨论 `recv()` 时，假定请求读取的长度大于零：

- `recv > 0`：本次读到这些字节，消息是否完整仍按阶段二判断。
- `recv == 0`：对端已正常结束其发送方向，且内核接收缓冲中的已有数据已被读完，表示 EOF；这不代表用户态 `BufferReader` 中的数据已被业务消费。
- `recv < 0`：结合 `errno` 区分 `EAGAIN/EWOULDBLOCK`、`EINTR` 和其他错误。
- `send` 可以只接受部分字节；暂时写不出与永久错误需要分别处理。

这些是系统调用的语义，见 [recv(2)](https://man7.org/linux/man-pages/man2/recv.2.html)、[send(2)](https://man7.org/linux/man-pages/man2/send.2.html)。本项目的封装还有自己的返回分支，不能把二者直接等同。

## 2. 连接与缓冲区分别由谁持有

### 2.1 一条连接对应哪些对象

`TcpServer` 的 `connects_` 表按连接 fd 保存 `shared_ptr<TcpConnection>`。每个 `TcpConnection` 主要保存：

| 成员 | 类型与作用 |
| --- | --- |
| `channel_` | `shared_ptr<Channel>`，保存连接 fd、关注事件与回调 |
| `read_buffer_` | `unique_ptr<BufferReader>`，持有接收字节的 vector |
| `write_buffer_` | `unique_ptr<BufferWriter>`，持有待发送的队列项 |
| `task_schduler_` | 借用的调度器指针，负责该连接的事件注册 |
| `mutex_` | 用于当前代码中的部分读写、排队和关闭操作 |

内核 socket 缓冲区与这两个 C++ 缓冲对象是不同的存储区域：

```text
接收：
内核接收缓冲 → recv → BufferReader 的 vector → 业务回调

发送：
业务数据 → BufferWriter 的队列 → send → 内核发送缓冲
         → 网络 → 对端接收与处理
```

事件回调的线程归属复用[阶段三](阶段三epoll与事件循环学习讲义.md)。本讲进一步核对：某次方法调用由谁发起，以及调用时数据保存在哪里。

### 2.2 主例只假定一条完整请求

沿用阶段二的 14 字节 `JOIN`。本例假定完整、合法的请求已到达，业务处理准备了一条当前布局下为 8 字节的 `JoinReply_body`；信令状态和登记结果在阶段七展开。

当前小端环境中的成功应答可表示为：

```text
08 00 | 05 00 | 00 00 00 00
 len  | JOIN  | result = 0
```

后文“第一次只写出 3 字节”是固定的手推输入，不要求通过真实网络制造这个结果。

## 3. 读入、解析与消费怎样衔接

### 3.1 HandleRead 的锁范围和业务回调

[TcpConnection::HandleRead()](../EdoyunNet/TcpConnection.cpp) 的当前顺序是：

```text
连接读就绪
  → 加锁，检查 is_closed_
  → BufferReader::Read(fd)，尝试一次 recv
  → 若返回负数，调用 Close 并返回
  → 离开这段锁的作用域
  → 调用业务 readCb_(连接, 接收缓冲)
  → 若业务回调返回 false，重新加锁并调用 Close
```

业务回调执行前，读操作持有的连接锁已经释放。因此，在这条正常路径中，业务回调调用 `TcpConnection::Send()` 时，可以重新获取同一个连接锁。

`readCb_` 返回 true 只表示不通过这个返回值要求关闭连接，不代表缓冲区已经消费完毕。消费由业务处理器显式调用 `Retrieve()` 或 `ReadAll()` 完成。

### 3.2 一次读取怎样改变索引

`BufferReader::Read()` 先确保尾部有可写空间，再执行：

```cpp
int bytes_read = ::recv(fd, BeginWrite(), MAX_BYTES_PER_READ, 0);
if (bytes_read > 0)
{
    writer_index_ += bytes_read;
}
```

可读区仍是阶段二的 `[reader_index_, writer_index_)`。若初始索引均为 0，本次读到完整 JOIN 的 14 字节：

| 动作 | reader / writer | 可读字节数 |
| --- | --- | --- |
| 读取前 | 0 / 0 | 0 |
| `recv` 返回 14 | 0 / 14 | 14 |
| 业务查看 `Peek()` | 0 / 14 | 14 |
| 完整消息处理后 `Retrieve(14)` | 0 / 0 | 0 |

`Peek()` 不消费；`Retrieve()` 改变可读范围。半包、多包与非法长度继续按[阶段二讲义](阶段二TCP字节流与自定义协议学习讲义.md)推演，不在这里重写解析器。

### 3.3 扩容不等于接收到更多数据

当前默认 `buffer_.size()` 为 2048，而单次读取上限是 4096。第一次 `Read()` 在调用 `recv` 前就发现尾部不足 4096，于是把 size 增加到 6144；实际收到多少仍看 `recv` 的返回值。

扩容时只增加尾部空间，不把已消费的前缀搬回。例如：

```text
buffer_.size() = 6144
reader = 5000，writer = 6000

可读：1000 字节
尾部可写：144 字节
前缀已消费：5000 字节

下一次 Read：
尾部不足 4096 → resize 到 10240
reader 和 writer 先保持不变，再把新数据接到 writer 位置
```

`Size()` 返回 vector 的 size，不是可读量，也不是 `vector::capacity()`。`RetrieveAll()` 只清零索引，不缩小 vector。

`resize` 可能触发重新分配，使此前保存的 `Peek()` 指针失效；发生可能扩容的操作后，应重新取得指针。相关容器规则见 [vector 容量与重分配](https://eel.is/c++draft/vector.capacity)。数据即使暂未被擦除，消费后也已经退出有效可读区，未来复用缓冲区时可能被覆盖。

### 3.4 ReadAll 读取的是哪一层

`ReadAll(out)` 只把当前用户态可读区复制到 `std::string`，不会继续调用 `recv`：

- 当前有数据时，按明确长度复制到 `out`，再清零两个索引；内容可以包含零字节。
- 当前无数据时，返回 0，但不会清空 `out` 原来的内容。

[TCPServer/LConnection.cpp](../TCPServer/LConnection.cpp) 的 `HandleMessage()` 使用它后再发送字符串。这适合对照复制路径，但“取出当前所有字节”不等于“得到一条完整协议消息”。

该演示不在默认构建中；`EdoyunNet/main.cpp` 也没有默认可执行目标。本阶段可以直接阅读，不需要另外启用演示。

### 3.5 返回值应有含义与当前处理

| 情况 | 应区分的含义 | 当前实现 |
| --- | --- | --- |
| 实际 `recv` 返回正数 | 收到字节 | 推进 writer，再进入业务回调 |
| 实际 `recv` 返回 0 | TCP 接收方向 EOF | `HandleRead` 没有专门关闭分支，仍可能进入业务回调 |
| `recv` 返回 -1，`errno=EAGAIN/EWOULDBLOCK` | 当前暂时不可读 | `HandleRead` 把负数一律交给 `Close` |
| `recv` 返回 -1，`errno=EINTR` | 本次读取被信号中断 | 同样没有单独区分 |
| 其他负数错误 | 需要按错误处理连接 | 当前进入 `Close` |

`BufferReader::Read()` 的容量保护分支也会直接返回 0，而且没有调用 `recv`。因此，封装返回 0 不能无条件解释为网络 EOF。

上限检查使用的是扩容前的 `buffer_.size() > MAX_BUFFER_SIZE`，之后才增加大小；它不是对新大小的严格上限检查。本阶段先解释这些分支，完整错误恢复和关闭时序留后续检查。

## 4. 准备应答后，数据怎样进入写队列

### 4.1 两种 Send 重载分别保存什么

两个 `TcpConnection::Send()` 重载都先调用 `BufferWriter::Append()`，但保存数据的方式不同：

| 入口 | 入队时的操作 | 调用者需要理解的存活条件 |
| --- | --- | --- |
| `Send(char const*, size)` | 分配自己的存储，复制 `size` 字节 | 入队成功并复制后，原数据可以离开作用域 |
| `Send(shared_ptr<char>, size)` | 队列共同持有原来的存储，不复制内容 | 存储能继续存活，但调用者仍应避免改写尚未发送的数据 |

原始指针版本实际分配 `size + 512` 字节，并用数组 deleter 管理；只复制、发送 `size` 字节，额外 512 字节不会成为消息内容。

共享版本省去这一次应用层数据复制，但不冻结内容，也不检查传入长度是否匹配实际存储大小。队列出队时释放自己持有的引用，底层存储是否随即释放，还取决于是否有其他所有者。

### 4.2 栈上应答为何能留下待发送副本

[SigConnection::HandleJion()](../SigServer/SigConnection.cpp) 中的 `reply_body` 是局部对象，发送入口为：

```cpp
this->Send((char const*)&reply_body, reply_body.len);
```

它走复制版本。即使底层第一次只接受 3 字节，`HandleJion()` 返回、局部 `reply_body` 销毁后，写队列仍保存着剩余应答所需的副本。

沿主例核对数据去向：

```text
读缓冲中的完整 JOIN
  → 业务处理，准备局部 reply_body
  → Send 的复制版本把应答存入写队列
  → 业务处理返回，接收端消费 JOIN 的 14 字节

应答是否仍在写队列中，取决于发送进度
```

接收消费与应答排空是两项独立进度。这里能安全释放原始局部应答，依赖的是入队时已复制；不能把这个结论套到借用原始内存的其他接口上。

### 4.3 入队也可能失败

`BufferWriter::Append()` 遇到 `size <= index` 或队列已满会返回 false。当前连接创建的是 `BufferWriter(500)`，但 `TcpConnection::Send()`：

1. 返回类型是 `void`；
2. 没有检查 `Append()` 的返回值；
3. 随后仍尝试发送已有队列中的数据；
4. 若连接已标记关闭，则直接不入队。

因此，业务发送函数返回，不能证明本次数据已经被接受。后续查看队列变小，也不能反过来证明刚才那条新数据曾经入队。

## 5. 部分发送与可写事件怎样推进

### 5.1 Packet 保存的是哪段进度

`BufferWriter` 使用 `std::queue<Packet>`，每个队列项保存：

| 字段 | 含义 |
| --- | --- |
| `data` | 持有待发送数据的 `shared_ptr<char>` |
| `size` | 这份数据的总长度 |
| `writeIndex` | 已被底层 socket 接受的前缀长度 |

默认从 `writeIndex=0` 开始。实际发送参数是：

```cpp
ret = ::send(sockfd, pkt.data.get() + pkt.writeIndex, pkt.size - pkt.writeIndex, 0);
```

- 返回正数时，`writeIndex` 增加该数值。
- 只发送部分数据时，保留队首和新索引，本次 `BufferWriter::Send()` 随即返回。
- 队首全部发送后，才将它出队，并可以继续处理下一个队列项。
- 队首未完成时，不会跳过它先发送后面的项，保持 FIFO 顺序。

一个队列项对应一次成功的 `Append`，不一定对应一个 TCP 包，也不必然对应一条完整业务消息。

### 5.2 HandleWrite 怎样决定关注写事件

[TcpConnection::HandleWrite()](../EdoyunNet/TcpConnection.cpp) 的正常路径是：

```text
检查连接是否已关闭
  → try_lock 成功
  → BufferWriter::Send(fd)
  → 若返回负数，调用 Close
  → 否则检查 IsEmpty()
      ├─ 非空且尚未关注写事件：EnableWriting → UpdateChannel
      └─ 空且原来关注写事件：DisableWriting → UpdateChannel
```

这里改变的是写关注位，读关注位仍保留。数据未排空时，后续写就绪回调再调用 `HandleWrite()`；队列空后取消写关注，避免在 LT 下持续收到无用的可写通知。

### 5.3 用 8 字节应答手推先写出 3 字节

假定应答成功入队，连接未关闭，锁获取成功，事件更新也成功。开始时只有读关注：

| 动作或底层结果 | 队列状态 | `BufferWriter::Send` 的返回值 | 写关注 |
| --- | --- | --- | --- |
| 入队 8 字节应答 | 一个队列项，`writeIndex=0` | 尚未调用 | 尚未开启 |
| `TcpConnection::Send` 立即尝试写，底层返回 3 | `writeIndex=3`，剩余 5 字节 | 3 | `HandleWrite` 开启 |
| 后续写就绪，底层返回 -1、`errno=EAGAIN` | 索引仍为 3，队列项保留 | 当前实现转换为 0 | 保持 |
| 再次写就绪，底层接受剩余 5 字节 | 队列项出队，本地队列为空 | 随后检查空队列并返回 0 | `HandleWrite` 取消 |

第二次发送从 `data + 3` 开始，不应重新发送前 3 字节。`EAGAIN` 那一行只是补充手推条件，可以先推“3 字节 → 剩余 5 字节”再回看。

若应答后面还排着另一项 Q，必须先排空当前应答，才轮到 Q。同一次 `BufferWriter::Send()` 可以完整发送多个队列项；遇到部分发送或暂时写不出时，才保留进度返回。

### 5.4 返回 0 为什么不能单独解释

当前 `BufferWriter::Send()` 将 `EINTR/EAGAIN` 转成 0；完整排空队列后，再次检查到队列为空时也返回 0。它的返回值不是本次累计发送字节数。

因此，上表最后一行虽然返回 0，底层本次实际接受了 5 字节；而前一行返回 0 时队列仍有 5 字节。`HandleWrite()` 必须结合 `IsEmpty()` 判断后续是否还需写关注。

若底层错误通过负返回值传回，当前 `HandleWrite()` 会调用 `Close()`。另需区分 `SIGPIPE`：当前 `send(..., 0)` 没有设置 `MSG_NOSIGNAL`；若进程没有另行处理该信号，某些发送失败可能使进程终止，并非都能走到 `Close` 分支，见 [send(2)](https://man7.org/linux/man-pages/man2/send.2.html)。本阶段不增加信号实验。

## 6. 谁来继续写，谁来限制生产

### 6.1 业务 Send 会立即尝试写

当前两个 `TcpConnection::Send()` 都在入队后释放锁，并直接调用 `HandleWrite()`。执行线程就是此次业务调用的线程，不会自动投递到连接所属调度器。

`HandleWrite()` 调用 `mutex_.try_lock()` 未能取得锁时，会立即返回；这条分支不会发送数据、开启写关注或安排重试任务：

- 已有写关注时，后续 LT 可写通知仍可能推进。
- 另一个写操作正持锁时，它也可能已经负责推进队列。
- 若此前没有写关注，且锁只是被非写路径暂时占用，就不能保证排队数据一定会自动得到下一次写触发。

这里要找到明确的后续调用者，不能只用“有锁，所以安全”解释整个推进过程。跨线程状态、重入和完整关闭审计留阶段十一。

### 6.2 队列上限限制什么

`BufferWriter` 的默认上限是 10000，但 `TcpConnection` 显式使用 500。`Size()` 返回队列项数量，`IsFull()` 按这个数量判断是否达到上限。

500 项可以分别承载很小或很大的数据，所以它不是待发字节数的上限。若每项业务数据分别是 1 KiB 或 1 MiB，同样的项数限制就可能对应相差 1024 倍的数据量；实际内存还涉及分配冗余和共享存储。

`TcpConnection` 还通过 `SocketUtil` 请求设置 `SO_SNDBUF=100*1024`，那是内核 socket 发送缓冲的设置，与用户态 500 个队列项是两层限制，实际内核取值由系统决定。

### 6.3 背压需要把压力传回生产端

当业务持续产生数据的速度高于 socket 接受数据的速度时，用户态队列会积累。TCP 的流量控制可以使底层暂时写不出，但不会替应用停止生产或停止入队。

完整的背压安排需要让生产者知道当前无法继续接受数据，并按业务选择减速、暂停、明确拒绝或丢弃；若采用暂停，还要有恢复条件。

当前实现只有队列项上限，缺少：

- 本次入队失败向调用者反馈；
- 按总排队字节或水位约束积压；
- 与生产端配合的暂停、恢复或其他明确策略。

所以应把它理解为“有上限但反馈不足的发送队列”。本阶段能说明缺口与影响即可，不要求把所有策略都实现。

## 7. 沿现有路径核对一次

先在源码中标出主例的四个位置：

1. `HandleRead` 调用读缓冲，业务回调开始前释放连接锁。
2. `SigConnection::HandleJion` 准备应答并调用复制版 `Send`。
3. `BufferWriter::Append` 保存数据，`BufferWriter::Send` 推进 `writeIndex`。
4. `TcpConnection::HandleWrite` 根据队列是否为空更新写关注。

服务已能正常运行、且已有合法收发时，用断点或已有日志跟一次即可。实际发送经常一次就完成；部分发送按第五节的固定数字手推，不必制造慢读或强求命中每个分支。

阶段二已经指出当前信令半包循环的问题，本讲的完整 JOIN 主例不代表它已被修复。运行遇到这个阻断时，先保留源码结论，继续其他独立内容。

没有现成信令运行条件时，也可以在已可运行的其他服务上核对相同的连接读写入口。`TCPServer/LConnection` 只作现有代码对照，完整请求响应与定时器安排在阶段五。

关闭入口也只先区分位置：`TcpConnection::Close()` 标记关闭、移除 Channel 并通知回调；它不立即清空写队列，也不直接调用系统 `close(fd)`。最终 fd 和缓冲资源的释放，要结合连接对象是否被销毁判断，完整顺序留阶段十一。

## 8. 自检与下一步

用源码与固定应答回答：

1. `recv` 返回 14 后，谁推进 writer，谁决定消费 JOIN 的 14 字节？
2. 缓冲区从 2048 扩到 6144，是否意味着读到了 6144 字节？已消费前缀会自动搬回吗？
3. 局部 `reply_body` 离开作用域后，为什么复制版发送仍能保留未发出的 5 字节？
4. 共享指针版入队后，原存储能存活，是否意味着调用者可以任意改写它？
5. 底层先返回 3、再返回 5 时，`writeIndex` 怎样变化？为什么最后一次 `BufferWriter::Send()` 返回 0，也可能已经排空本地队列？
6. 队列达到 500 项、或 `try_lock()` 失败时，当前接口给了调用者哪些保证，哪些没有保证？

完成标准：

- [ ] 能解释一次读入、业务消费与未完成字节的保留，区分系统调用语义与当前错误分支。
- [ ] 能指出两种发送入口的复制点、所有者和存活条件。
- [ ] 能推演部分发送、FIFO 与写关注变化，区分入队、本机 socket 接受和对端处理。
- [ ] 能说明当前队列上限、失败反馈及后续推进的缺口。

随后进入[阶段五：定时器与最短业务闭环](项目学习规划.md#stage-5)，把本讲的连接收发与一个现有请求响应、定时回调联系起来。
