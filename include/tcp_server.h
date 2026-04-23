#pragma once
#include "lru_store.h"
#include "resp_parser.h"
#include <functional>
#include <string>

namespace ErrMsg {
    const std::string INVALID_DB_INDEX  = "INVALID DB INDEX";
    const std::string INVALID_ARGS      = "WRONG NUMBER OF ARGUMENTS";
    const std::string INVALID_TTL       = "TTL MUST BE A VALID INTEGER";
    const std::string MAX_CLIENTS       = "MAX CLIENTS REACHED, TRY AGAIN LATER";
    const std::string UNKNOWN_COMMAND   = "UNKNOWN COMMAND";
};

class TCPServer {
    struct CommandDef {
        int expectedArgs;
        std::function<std::string(const RESPCommand&)> handler;
    };

    int serverFd;
    int port;
    std::vector<LRUStore>& stores;
    std::vector<std::thread> workerThreads;
    std::thread acceptThread;
    std::queue<int> clientFds;
    std::mutex queueMtx;
    std::condition_variable clientCv;
    std::atomic<int> activeClients{0};
    std::atomic<bool> shouldStop{false};
    RESPParser parser;
    std::unordered_map<std::string, CommandDef> commandRegistry;

    bool isInvalidDBIndex(const RESPCommand& cmd);
    void initCommandRegistry();
    std::string handleCommand(const RESPCommand& cmd);
    void handleClient(int fd);
    void acceptLoop();
    void workerLoop();


public:

    TCPServer(std::vector<LRUStore>& _stores, int _port);
    ~TCPServer();
    bool start();
    bool stop();

};