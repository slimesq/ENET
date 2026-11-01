
#include <stdio.h>
#include "LoadBanceServer.h"
#include "../EdoyunNet/EventLoop.h"

int main()
{
    EventLoop loop(1);
    auto loadServer = LoadBanceServer::Create(&loop);
    if (!loadServer->Start("172.21.205.121", 8523))
    {
        printf("LoadBanceServer server failed\n");
        return 1;
    }
    printf("LoadBanceServer server successful\n");
    getchar();
    return 0;
}
