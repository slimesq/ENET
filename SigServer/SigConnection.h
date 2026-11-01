#include <map>
#include <vector>
#include <cstdint>
#include "define.h"
#include "../EdoyunNet/TcpConnection.h"

class SigConnection : public TcpConnection
{
public:
    SigConnection(TaskScheduler* scheduler, int socket);
    ~SigConnection();

public:
    bool IsAlive()
    {
        return state_ != CLOSE;
    }
    bool IsNoJion()
    {
        return state_ == NONE;
    }
    bool IsIdle()
    {
        return state_ == IDLE;
    };
    bool IsBusy()
    {
        return (state_ == PUSHER || state_ == PULLER);
    };
    void DisConnected();
    void AddCustom(std::string const& code);
    void RmoveCustom(std::string const& code);
    RoleState GetRoleState() const
    {
        return state_;
    };
    std::string GetCode() const
    {
        return code_;
    };
    std::string GetStreamAddres() const
    {
        return streamAddres_;
    };

protected:
    bool OnRead(BufferReader& buffer);
    void HandleMessage(BufferReader& buffer);
    void Clear();

private:
    void HandleJion(packet_head const* data);
    void HandleObtainStream(packet_head const* data);
    void HandleCreateStream(packet_head const* data);
    void HandleDeleteStream(packet_head const* data);
    void HandleOtherMessage(packet_head const* data);

private:
    void DoObtainStream(packet_head const* data);
    void DoCreateStream(packet_head const* data);

private:
    RoleState state_;
    std::string code_;
    std::string streamAddres_;
    TcpConnection::Ptr conn_ = nullptr;
    std::vector<std::string> objectes_;
};