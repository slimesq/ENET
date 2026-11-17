# ENET 阶段三：epoll 与事件循环学习讲义

> 对应[项目学习规划的阶段三](项目学习规划.md#stage-3)。参考用时：6～8 小时，基础补学、环境配置和排障另计。
>
> 复用已有 Linux socket/epoll 知识，重点解释本项目由谁创建线程、由谁注册 fd、就绪事件怎样找到 Channel，以及回调实际在哪个线程执行。

## 1. 学习顺序与局部补缺

按以下顺序阅读：

1. 快速核对 `TcpSocket` 的封装、非阻塞设置及 fd 归属。
2. 沿 `EventLoop::Loop → std::thread → TaskScheduler::Start` 找到真正执行循环的线程。
3. 阅读 Channel 的关注事件与调度器的 ADD、MOD、DEL。
4. 跟踪 `epoll_wait → Channel::HandleEvent → OnAccept`，再看新连接分配到哪里。
5. 核对 LT、零超时循环和停止标志，最后按运行条件观察一次接入。

主要源码：

| 位置 | 本阶段看什么 |
| --- | --- |
| [TcpSocket.h](../EdoyunNet/TcpSocket.h)、[TcpSocket.cpp](../EdoyunNet/TcpSocket.cpp) | socket 封装与 `SocketUtil`；后者也定义在这两个文件中 |
| [EventLoop.h](../EdoyunNet/EventLoop.h)、[EventLoop.cpp](../EdoyunNet/EventLoop.cpp) | 创建线程、持有调度器、选择调度器与基本退出顺序 |
| [TaskScheduler.h](../EdoyunNet/TaskScheduler.h)、[TaskScheduler.cpp](../EdoyunNet/TaskScheduler.cpp) | 工作循环、停止标志、定时器调用位置 |
| [EpoolTaskScheduler.h](../EdoyunNet/EpoolTaskScheduler.h)、[EpoolTaskScheduler.cpp](../EdoyunNet/EpoolTaskScheduler.cpp) | epoll fd、Channel 表、注册和事件分发 |
| [Channel.h](../EdoyunNet/Channel.h) | fd、关注事件、读写及关闭回调 |
| [Acceptor.cpp](../EdoyunNet/Acceptor.cpp)、[TcpServer.cpp](../EdoyunNet/TcpServer.cpp) | 监听接入与新连接交接 |
| [TcpConnection.cpp](../EdoyunNet/TcpConnection.cpp) | 连接 Channel 怎样注册到选中的调度器 |

文件名保留了 `EpoolTaskScheduler` 的拼写，类名则是 `EpollTaskScheduler`，检索时分别使用真实名称。

### 1.1 并发知识只补当前缺口

能解释下表内容时，直接进入第二节；有疑问时，先结合短片段确认对应概念。

| 概念 | 本阶段需要理解到什么程度 |
| --- | --- |
| `std::thread` | 用可调用对象构造线程对象后，新线程即可开始执行；仅凭源码中的排列顺序，不能确定两个线程中语句的实际执行先后 |
| `join()` 与 `joinable()` | `join()` 等待线程结束；`joinable()` 返回 true 不代表线程仍在运行，线程执行结束但尚未 join 时，该函数仍可能返回 true |
| 原子停止标志 | 原子变量适合在不同线程间读写停止请求，但不自动保护其他成员或容器 |
| 对象存活 | 线程通过原始指针访问调度器时，调度器对象必须保持存活；回调执行期间，`this` 指向的对象也必须保持存活 |

线程 API 可按需查 [std::thread 文档](https://learn.microsoft.com/en-us/cpp/standard-library/thread-class?view=msvc-170)。回调保存与捕获的基本关系复用[阶段一讲义](阶段01_已有基础复用与源码阅读方法学习讲义.md)。

### 1.2 按需观察线程启动与停止

对上表仍有疑问时，可在 Linux／WSL 终端运行以下 C++17 片段。它只演示标准库的线程与原子标志，不依赖 ENET 或网络环境。

```sh
threadDir="$(mktemp -d)"
c++ -std=c++17 -pthread -x c++ - -o "$threadDir/EnetThreadBasics" <<'CPP'
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    std::atomic_bool stopRequested{false};
    std::thread worker{[&stopRequested]() {
        std::puts("worker started");
        while (!stopRequested.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        std::puts("worker finished");
    }};

    std::puts("main requests stop");
    stopRequested.store(true);
    worker.join();
    std::puts("main joined");
}
CPP
"$threadDir/EnetThreadBasics"
```

`worker started` 与 `main requests stop` 的打印顺序可以不同；`main joined` 一定在工作线程结束之后打印。即使主线程在工作线程进入 lambda 之前就设置了停止标志，工作线程仍会执行 lambda，只是在检查标志后跳过循环并结束。

`stopRequested` 是 `main()` 中的局部变量，lambda 按引用捕获它。主线程先通过 `join()` 等待工作线程结束，再离开变量作用域，因此工作线程使用的引用始终有效。

这里用原子变量读写停止请求；ENET 中的 Channel 表、连接状态等仍需分别检查同步方式。本阶段不展开内存序课程。

## 2. 先认清对象与 fd 的职责

### 2.1 快速核对 socket 封装

沿 [TcpSocket.cpp](../EdoyunNet/TcpSocket.cpp) 看当前实现：

| 操作 | 当前行为与阅读重点 |
| --- | --- |
| `Create()` | 调用 `socket(AF_INET, SOCK_STREAM, 0)`，返回并保存 fd |
| `Bind()` | 填充地址，对端口调用 `htons()`，返回绑定是否成功 |
| `Listen()` | 调用 `listen()`，返回是否成功 |
| `Accept()` | 调用一次 `accept()`，返回新连接 fd 或错误 |
| `SocketUtil::SetNonBlock()` | 用 `fcntl` 设置 `O_NONBLOCK`；当前没有完整检查这两次 `fcntl` 的失败 |
| `Close()` | 显式关闭 fd，并把保存的值设为 -1 |

监听 fd 在 `Acceptor::Listen()` 中设置非阻塞；接入后的连接 fd 在 `TcpConnection` 构造函数中再次设置。Linux 的 `accept()` 不会自动把监听 socket 的 `O_NONBLOCK` 状态继承给新 socket，见 [accept(2)](https://man7.org/linux/man-pages/man2/accept.2.html)。

这里区分三种 fd：epoll fd 标识一个 epoll 实例，监听 fd 用于接入连接，新连接 fd 用于收发数据。

`Channel` 保存 socket fd、关注事件与回调，析构时不关闭 fd。监听 fd 通过 `Acceptor::Close → TcpSocket::Close` 显式关闭，连接 fd 最终由 `TcpConnection` 析构关闭；完整关闭时序在阶段十一核对。

### 2.2 每个对象负责什么

| 对象 | 在本项目中的职责 |
| --- | --- |
| `EventLoop` | 创建并持有多个调度器和工作线程，提供 Channel、定时器与调度器选择接口 |
| `TaskScheduler` | 在线程入口 `Start()` 中反复处理定时器和 I/O，保存本调度器的定时器队列与停止标志 |
| `EpollTaskScheduler` | 继承 TaskScheduler，持有自己的 epoll fd 和 `channels_` 表，实现事件注册与分发 |
| `Channel` | 保存一个 socket fd、关注事件和回调，将本次就绪事件分发给对应回调 |
| `Acceptor` | 准备监听 socket，在监听读事件回调中接受连接 |
| `TcpConnection` | 持有连接 Channel 和收发缓冲区，保存选中的调度器指针 |

以两个调度器为例，关系是：

```text
主线程
  → 构造 EventLoop
      ├─ 持有 scheduler[0] 与工作线程 0
      │    └─ 线程 0 运行 Start：定时器队列 0 + epoll fd 0 → 本调度器的 Channel
      └─ 持有 scheduler[1] 与工作线程 1
           └─ 线程 1 运行 Start：定时器队列 1 + epoll fd 1 → 本调度器的 Channel
```

图中的 0、1 是逻辑编号，不是操作系统分配的 fd 数值或线程 ID。每个调度器创建自己的 epoll 实例；多个线程并非共同轮询同一个 epoll fd。

## 3. 工作线程从哪里开始执行

### 3.1 Loop 在这里负责启动线程

[EventLoop 构造函数](../EdoyunNet/EventLoop.cpp)立即调用 `Loop()`。`Loop()` 对每个编号创建一个 `EpollTaskScheduler`，保存到容器，再用 `std::thread` 启动 `TaskScheduler::Start`。

因此，`EventLoop::Loop()` 在这里负责创建并启动工作线程；持续处理事件的循环在 `TaskScheduler::Start()` 中。`Loop()` 返回后，主线程继续创建服务，并调用 `TcpServer::Start()` 建立监听。

`native_handle()` 只是取得原生线程句柄，不是启动线程的动作。线程在构造 `std::thread` 时已经启动。

实际服务入口中，负载、登录和信令显式使用 `EventLoop(1)`，RTMP 使用 `EventLoop(2)`。当前头文件的构造默认参数是 `uint32_t(-1)`，不能根据成员 `num_threads_` 声明处的初始值 1，推断无参构造只会启动一个线程；阅读和片段示例都以入口中的显式参数为准。

### 3.2 每一轮循环做什么

[TaskScheduler::Start()](../EdoyunNet/TaskScheduler.cpp) 的核心是：

```cpp
is_shutdown_ = false;
while (!is_shutdown_)
{
    //处理定时事件
    this->timer_queue_.HandleTimerEvent();
    //处理IO事件
    this->HandleEvent();
}
```

同一调度器的定时器处理和 I/O 分发在同一个工作线程中顺序执行。这里调用的是 `TaskScheduler` 的 `HandleEvent()` 虚接口，实际执行 `EpollTaskScheduler::HandleEvent()`。

I/O 回调也在这条调用链上同步执行；没有额外的业务任务队列把回调自动转交给另一组线程。慢回调会延迟同一调度器上的其他事件和定时器。

## 4. Channel 怎样注册到 epoll

### 4.1 关注事件与就绪事件分别看

在 [Channel.h](../EdoyunNet/Channel.h) 中：

- 成员 `events_` 保存应用希望关注的事件，`EnableReading()`、`EnableWriting()` 修改它。
- `GetEvents()` 返回关注事件，供 `epoll_ctl` 使用。
- `HandleEvent(int events)` 的参数则是 `epoll_wait` 本次返回的就绪事件。

改变 `events_` 后，还要调用调度器的 `UpdateChannel()`，把新的事件关注设置提交给 epoll。仅调用 `EnableWriting()` 不会自动执行 `epoll_ctl`。

项目中 `EVENT_IN/PRI/OUT/ERR/HUP` 的数值分别为 1、2、4、8、16，与对应 Linux epoll 事件位一致；当前代码直接传递这些位，没有额外的转换层。

### 4.2 ADD、MOD、DEL 怎样选择

[EpollTaskScheduler::UpdateChannel()](../EdoyunNet/EpoolTaskScheduler.cpp) 先查询 `channels_` 是否已有该 fd：

| 表中是否已有 fd | Channel 是否仍关注事件 | 当前操作 |
| --- | --- | --- |
| 没有 | 有关注事件 | 保存 Channel，并执行 `EPOLL_CTL_ADD` |
| 已有 | 有关注事件 | 执行 `EPOLL_CTL_MOD` |
| 已有 | 没有关注事件 | 执行 `EPOLL_CTL_DEL`，再从表中移除 |
| 没有 | 没有关注事件 | 不注册 |

显式调用 `RmoveChannel()` 也会在找到 fd 后执行 DEL 并移除表项。方法名中的拼写按源码保留。

这些操作用互斥锁保护 `channels_` 的修改，但当前 `epoll_ctl` 失败只打印提示，没有把失败完整传回上层，也没有回滚先前的表项变化。下文的正常轨迹以相关系统调用成功为前提，不能只凭表中存在 Channel 就认定内核注册成功。

### 4.3 epoll 怎样找回 Channel

注册时，`Update()` 保存：

```cpp
event.data.ptr = Channel.get();
event.events = Channel->GetEvents();
```

`epoll_wait()` 返回相应事件时，应用从 `events[n].data.ptr` 取出原来的地址，再调用：

```cpp
((Channel*)events[n].data.ptr)->HandleEvent(events[n].events);
```

内核返回的是注册时保存的数据和本次就绪位，C++ 回调由用户态代码调用；具体语义见 [epoll_wait(2)](https://man7.org/linux/man-pages/man2/epoll_wait.2.html)。

这是 I/O 就绪通知：应用仍要在回调中调用 `accept/recv/send` 等操作。返回读就绪也不代表一条业务消息已经收齐，消息完整性仍按阶段二的协议规则判断；具体收发返回值在阶段四核对。

`channels_` 表通过 `shared_ptr<Channel>` 持有对象，`data.ptr` 只保存原始指针，不增加引用计数。从表中移除 Channel 后，已返回的事件仍可能保存它的地址；处理这些事件时，Channel 对象是否仍然存活，需要结合对象销毁和跨线程时序检查，留到阶段十一处理。

`Channel::HandleEvent()` 按就绪位依次检查读、写、挂断和错误。同一个 fd 的本次就绪结果可能同时包含多个事件位，因此可能触发多个回调；当前挂断分支调用关闭回调后会返回。

## 5. 跟一次新连接，确定执行线程

### 5.1 监听 Channel 固定交给 0 号调度器

[Acceptor::Listen()](../EdoyunNet/Acceptor.cpp) 在主线程的服务启动路径中创建监听 socket、设置非阻塞、绑定并监听，然后设置读回调并执行：

```text
Acceptor::Listen
  → EventLoop::UpdateChannel
  → task_schdulers_[0]->UpdateChannel
  → epoll_ctl(ADD)
```

监听就绪后，由调度器 0 的工作线程执行：

```text
epoll_wait
  → 监听 Channel::HandleEvent
  → Acceptor::OnAccept
  → accept 得到新连接 fd
  → TcpServer 构造时安装的新连接回调
  → 具体服务的 OnConnect
```

回调保存的基本原理已在[阶段一](阶段01_已有基础复用与源码阅读方法学习讲义.md)说明，这里重点确认调用线程与新连接去向。

### 5.2 新连接选择调度器，不等于立即切换线程

各服务的 `OnConnect()` 调用 `EventLoop::GetTaskSchduler()`，将选中的调度器指针传给连接构造函数。连接随后直接调用这个调度器的 `UpdateChannel()`，不再经过固定转发到 0 号的 `EventLoop::UpdateChannel()`。

`GetTaskSchduler()` 初始索引为 1，到末尾会回到 0：

| 调度器数量 | 连续调用的返回编号 |
| --- | --- |
| 1 | 0、0、0…… |
| 2 | 1、0、1、0…… |
| 3 | 1、2、0、1、2、0…… |

此表推演的是从初始状态开始连续调用的结果；若此前有其他代码调用过该函数，就应从当前索引继续计算。该索引的更新目前没有加锁，因此不能把这段轮转逻辑当作已完善的并发负载分配机制。

以两个调度器、首次选择 1 号为例：

| 动作 | 执行线程 |
| --- | --- |
| 处理监听事件、`OnAccept()`、业务 `OnConnect()` | 工作线程 0 |
| 构造新连接、设置回调和非阻塞、向调度器 1 注册 Channel | 仍是工作线程 0 |
| `OnConnect()` 返回，`TcpServer::AddConnection()` 保存连接 | 工作线程 0 |
| 后续由调度器 1 的 epoll 触发该连接的读写回调 | 工作线程 1 |

传入调度器指针只是确定注册归属，不会让正在执行的构造函数自动切换到另一个线程。Channel 注册后，另一个工作线程就可能开始处理它的就绪事件。因此，注册 Channel、完成业务派生对象的构造和把连接保存到连接表，并不是不可分割的一步；相关生命周期检查留到阶段十一。

0 号调度器也会被轮转选中，因此 0 号工作线程既接入连接，也可能处理普通连接事件。另需注意，`TcpConnection::Send()` 会直接调用 `HandleWrite()`；“epoll 触发的回调由所属工作线程执行”不等于“连接的所有方法都只由该线程调用”。发送路径在阶段四展开。

### 5.3 定时器归属取决于使用哪个接口

- 经 `EventLoop::AddTimer()` 添加的定时器，固定交给 0 号调度器。
- 直接对某个 `TaskScheduler` 调用 `AddTimer()`，则进入该调度器自己的队列。

例如 [RtmpConnection.cpp](../RtmpServer/RtmpConnection.cpp) 存在通过连接所属调度器添加定时器的调用。因此，不能概括为所有定时器都在 0 号线程；本阶段只认清接口归属，定时器内部逻辑留阶段五。

## 6. 当前循环方式意味着什么

### 6.1 当前是 LT，接入回调每次只 accept 一次

代码没有设置 `EPOLLET`，使用默认的 LT。`Acceptor::OnAccept()` 每次执行时只调用一次 `accept()`；当队列中仍有待接入连接、监听 fd 继续就绪时，后续轮次仍可报告事件。不能直接套用“ET 下循环调用 `accept()` 直到返回 EAGAIN”的要求来判断当前接入路径。

LT/ET 的 API 语义可查 [epoll(7)](https://man7.org/linux/man-pages/man7/epoll.7.html)。本阶段核对当前触发方式，接入批量化或 ET 改造按需深入。

`OnAccept()` 当前用 `fd > 0` 判断接入成功，而系统调用成功也可能返回 fd 0；同时，创建 socket 和设置属性的失败还缺少完整检查。这些是当前实现的边界，不是 epoll 的规则。

### 6.2 零超时为什么形成忙轮询

当前调用是：

```cpp
num_events = epoll_wait(epollfd_, events, 512, 0);
```

最后一个参数 0 表示没有事件也立即返回，见 [epoll_wait(2)](https://man7.org/linux/man-pages/man2/epoll_wait.2.html)。外层循环没有空闲休眠，于是不断执行“检查定时器 → 查询 I/O → 再循环”；这不是阻塞等待。

512 是本次事件数组能接收的事件数量上限，不是总连接数上限。空闲轮询通常会占用 CPU，具体数值需要实际观察。

此外，`EpollTaskScheduler::HandleEvent()` 可能返回 `false`，但 `TaskScheduler::Start()` 没有检查该返回值，循环不会因此自动结束。

### 6.3 停止请求、等待与唤醒

`TaskScheduler::Stop()` 只把原子停止标志 `is_shutdown_` 设为 true。调用 `EventLoop::Quit()` 的线程依次执行以下操作：

```text
对各调度器调用 Stop()
  → 调用各 std::thread 对象的 join()，等待对应工作线程结束
  → 清空调度器与线程容器
```

停止标志在外层 while 条件中检查。当前正在执行的定时器或 I/O 回调必须先返回，工作线程才有机会再次检查标志。阶段二的信令半包处理如果陷入循环，就会使处理它的工作线程无法再次检查停止标志。

零超时使空闲 I/O 查询立即返回，但不保证工作线程在所有情况下都能及时退出。`TaskScheduler::Start()` 还会先把停止标志重置为 false，因此启动与停止的完整时序也要在阶段十一核对。

若只把超时机械改成 -1，线程可能阻塞在 `epoll_wait` 中。当前没有配套的停止唤醒机制，定时器也依赖循环再次执行；要改等待策略，就需一起考虑下一次定时期限和主动唤醒方式。本阶段能解释原因即可，无需实现 eventfd、任务队列或完整关闭改造。

## 7. 按运行条件观察一次接入

复用阶段零的 Debug 产物。监听地址必须是实验环境中本机可绑定的地址，端口可用；环境准备见[阶段零讲义](阶段00_工程与构建基线学习讲义.md)。

服务可运行且已有调试工具时，围绕一条 TCP 连接观察：

1. 在 `TaskScheduler::Start()` 入口设置断点，查看工作线程及调用栈；需要核对创建者时，再看 `EventLoop::Loop()` 中的线程创建处。
2. 在 `Acceptor::OnAccept()` 中，区分监听 fd 与 `accept()` 返回的新连接 fd。
3. 在具体服务的 `OnConnect()` 和连接注册处，查看当前线程及选中的调度器编号，核对第五节的执行轨迹。
4. 已有正常数据收发时，再在 `TcpConnection::HandleRead()` 处确认所属线程；只建立连接而不发送数据时，不要求该断点命中。

可以用熟悉的 TCP 工具建立连接，不需要发送 JOIN、登录或 RTMP 命令。负载服务的单调度器入口适合先观察接入；两个调度器的分配可对照 RTMP 的现有网络入口做静态推演，不要求此时推流。

可用已有工具顺手观察服务空闲时的 CPU 占用，不做性能测试。服务暂不可运行时，先完成线程关系、注册与分配的源码推演，运行观察留待条件具备后补齐。

## 8. 自检与下一步

用源码回答以下问题：

1. `EventLoop::Loop()` 返回后，哪个函数仍在工作线程中循环？`native_handle()` 是否负责启动线程？
2. 启用两个调度器时，监听 Channel 在哪里，新连接的前几次选择是什么？
3. 第一个新连接选中 1 号调度器后，构造函数在哪个线程执行？后续 epoll 读回调又在哪个线程执行？
4. `Channel::events_` 与 `HandleEvent(events)` 的参数各表示什么？改变关注位后还要调用什么？
5. `channels_` 持有 `shared_ptr<Channel>`，为什么仍要检查 `data.ptr` 所指的 Channel 对象是否存活？
6. 零超时为什么会忙轮询？为什么只把超时改成 -1 还不够？

完成标准：

- [ ] 能追踪一次“注册 Channel → epoll 返回事件 → 通过指针分发回调”，区分监听 fd、连接 fd 和 epoll fd。
- [ ] 能说明监听、新连接与两种定时器接口的调度器归属，区分构造时的当前线程与后续事件线程。
- [ ] 能解释当前 LT、零超时循环、停止标志及 join 的关系，并指出仍需阶段十一核对的生命周期问题。

随后进入[阶段四：TCP 连接、收发缓冲区与背压](项目学习规划.md#stage-4)，继续跟踪读写回调如何使用缓冲区、消费消息以及推进发送。
