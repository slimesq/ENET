#ifndef _ORMMANAGER_H_
#define _ORMMANAGER_H_
#include <memory>
#include <mutex>
#include <mysql.h>

class ORMManager
{
public:
    ~ORMManager();
    static ORMManager* GetInstance();  //全局实例
    void UserRegister(char const* name,
                      char const* acount,
                      char const* password,
                      char const* usercode,
                      char const* sig_server);
    MYSQL_ROW UserLogin(char const* usercode);
    void UserDestroy(char const* usercode);
    void insertClient(char const* name,
                      char const* acount,
                      char const* password,
                      char const* usercode,
                      int online,
                      long recently_login,
                      char const* sig_server);

protected:
    void deleteClientByUsercode(char const* usercode);
    MYSQL_ROW selectClientByUsercode(char const* usercode);

private:
    ORMManager();
    MYSQL mysql_;

private:
    static std::unique_ptr<ORMManager> instance_;
};
#endif
