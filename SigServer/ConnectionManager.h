#include <unordered_map>
#include <memory>
#include <mutex>
#include "../EdoyunNet/TcpConnection.h"

class ConnectionManager
{
public:
    ~ConnectionManager();
    static ConnectionManager* GetInstance();

public:
    void AddConn(std::string const& idefy, TcpConnection::Ptr const conn);
    void RmvConn(std::string const& idefy);
    TcpConnection::Ptr QueryConn(std::string const& idefy);
    uint32_t Size() const
    {
        return connMaps_.size();
    }

private:
    ConnectionManager();
    void Close();
    std::mutex mutex_;
    static std::unique_ptr<ConnectionManager> instance_;
    std::unordered_map<std::string, TcpConnection::Ptr> connMaps_;
};