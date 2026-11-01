#include "../EdoyunNet/TcpConnection.h"
#include "define.h"

class LoginConnection : public TcpConnection
{
public:
    LoginConnection(TaskScheduler* scheduler, int socket);
    ~LoginConnection();

protected:
    bool IsTimeout(uint64_t timestamp);
    bool OnRead(BufferReader& buffer);
    void HandleMessage(BufferReader& buffer);
    void Clear();

private:
    void HandleRegister(packet_head const* data);
    void HandleLogin(packet_head const* data);
    void HandleDestory(packet_head const* data);
};