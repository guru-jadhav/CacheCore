#include "../include/tcp_server.h"
#include "config.h"
#include "lru_store.h"
#include "resp_parser.h"
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

bool TCPServer::isInvalidDBIndex(const RESPCommand& cmd) {
    return (cmd.dbIndex < 0 || cmd.dbIndex >= stores.size());
};

/**
 * @brief Initialize the $O(1) routing table on startup
 */
void TCPServer::initCommandRegistry() {

    commandRegistry["GET"] = {1, [this](const RESPCommand& cmd) {
        auto result = stores[cmd.dbIndex]->GET(cmd.args[0]);
        if (!result) {
            return parser.serialize(ResponseType::NULLBULK, "");
        }
        return parser.serialize(ResponseType::BULK, *result);
    }};

    commandRegistry["SET"] = {3, [this](const RESPCommand& cmd) {
        bool result = stores[cmd.dbIndex]->SET(cmd.args[0], cmd.args[1], cmd.args[2] == "1");
        return parser.serialize(ResponseType::INTEGER, result ? "1" : "0");
    }};

    commandRegistry["DEL"] = {1, [this](const RESPCommand& cmd) {
        stores[cmd.dbIndex]->DEL(cmd.args[0]);
        return parser.serialize(ResponseType::OK, "");
    }};

    commandRegistry["EXISTS"] = {1, [this](const RESPCommand& cmd) {
        bool result = stores[cmd.dbIndex]->EXISTS(cmd.args[0]);
        return parser.serialize(ResponseType::INTEGER, result ? "1" : "0");
    }};

    commandRegistry["CLEAR"] = {0, [this](const RESPCommand& cmd) {
        stores[cmd.dbIndex]->CLEAR();
        return parser.serialize(ResponseType::OK, "");
    }};

    commandRegistry["EXPIRE"] = {2, [this](const RESPCommand& cmd) {
        try {
            stores[cmd.dbIndex]->EXPIRE(cmd.args[0], std::stoi(cmd.args[1]));
            return parser.serialize(ResponseType::OK, "");
        } catch(...) {
            return parser.serialize(ResponseType::ERROR, "invalid TTL value");
        }
    }};

    commandRegistry["INCR"] = {1, [this](const RESPCommand& cmd) {

        auto result = stores[cmd.dbIndex]->INCR(cmd.args[0]);
        if(!result){
            return parser.serialize(ResponseType::ERROR, "value is not an integer or out of range");
        }
        return parser.serialize(ResponseType::INTEGER, *result);
    }};
    
    commandRegistry["PING"] = {0, [this](const RESPCommand& cmd){
        std::string result = stores[cmd.dbIndex]->PING();
        return parser.serialize(ResponseType::SIMPLE_STRING, result);
    }};

};

std::string TCPServer::handleCommand(const RESPCommand& cmd) {
    if (isInvalidDBIndex(cmd)) {
        return parser.serialize(ResponseType::ERROR, ErrMsg::INVALID_DB_INDEX);
    }

    auto it = commandRegistry.find(cmd.command);
    if (it == commandRegistry.end()) {
        return parser.serialize(ResponseType::ERROR, ErrMsg::UNKNOWN_COMMAND);
    }

    // get the expectedArgs and lambda function for the specific command
    const auto& cmdDef = it->second;
    
    if (cmd.args.size() != cmdDef.expectedArgs) {
        std::string errMsg = ErrMsg::INVALID_ARGS + " — " + cmd.command + 
                                " expects " + std::to_string(cmdDef.expectedArgs) + 
                                " arg(s) but received " + std::to_string(cmd.args.size());
        return parser.serialize(ResponseType::ERROR, errMsg);
    }

    // execute mapped lambda function for the key
    return cmdDef.handler(cmd);
}

void TCPServer::handleClient(int fd){
    
    char buffer[4096];
    std::string accumulated;

    // as we have 1:1 connections for client and we never close the connectiosn we keep waiting for client
    while(true){

        int bytesRead = recv(fd, buffer, sizeof(buffer) - 1 , 0);

        // recv() == 0 -> it means connection closed by client
        if(bytesRead == 0){
            activeClients--;
            close(fd);
            return;
        }

        if(bytesRead < 0){
            std::cerr<<"recv() error: "<<strerror(errno)<<"\n";
            activeClients--;
            close(fd);
            return;
        }

        buffer[bytesRead] = '\0';
        accumulated += std::string(buffer, bytesRead);

        RESPCommand parsedCmd = parser.parse(accumulated);
        
        if (parsedCmd.status == ParseStatus::OK) {
            std::string response = handleCommand(parsedCmd);
            send(fd, response.c_str(), response.size(), 0);
            accumulated = "";
            continue;
        }
        
        // if the cmd is still to be revived from the network
        if (parsedCmd.status == ParseStatus::INCOMPLETE) {
            continue;
        }
        
        std::string errResponse = parser.serialize(ResponseType::ERROR, parsedCmd.errorMsg);
        send(fd, errResponse.c_str(), errResponse.size(), 0);
        accumulated = "";
    }
}

void TCPServer::acceptLoop() {
    while (!shouldStop) {
        sockaddr_in clientAddress;
        socklen_t clientLen = sizeof(clientAddress);
        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddress, &clientLen);

        if (clientFd < 0) {
            if(shouldStop) {
                // intentional shutdown — when we call .stop()
                return;  
            }
            std::cerr << "accept() failed: " << strerror(errno) << "\n";
            continue;
        }
        
        if(activeClients >= 10){
            std::string err = "-ERR max clients reached\r\n";
            send(clientFd, err.c_str(), err.size(), 0);
            close(clientFd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(queueMtx);
            clientFds.push(clientFd);
            activeClients++;
        }
        clientCv.notify_one();
    }
}

void TCPServer::workerLoop() {

    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(queueMtx);
        clientCv.wait(lock, [this]{
            return !clientFds.empty() || shouldStop;
        });

        if(shouldStop){
            break;
        }

        int fd = clientFds.front();
        clientFds.pop();
        lock.unlock();

        handleClient(fd);
    }
}

bool TCPServer::start(){

    serverFd = socket(AF_INET, SOCK_STREAM, 0);

    // return false if server socket creation fails -> socket() will return -1;
    if(serverFd < 0){
        return false;
    }
    
    // This tells the OS — "let me reuse this address even if it's in TIME_WAIT". 
    // So when you restart your server, it binds immediately without waiting 60 seconds.
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = INADDR_ANY;
    
    // try to bind the socket
    if(bind(serverFd, (struct sockaddr*)&address, sizeof(address)) < 0){
        std::cerr<<"bind() failed: "<<strerror(errno)<<"\n";
        close(serverFd);
        return false;
    }
    
    listen(serverFd, SOMAXCONN);
    acceptThread = std::thread(&TCPServer::acceptLoop, this);

    // create 10 worker thread -> 10 persistent connections
    workerThreads.reserve(10);
        for(int i = 0; i < 10; i++){
        workerThreads.emplace_back(std::thread(&TCPServer::workerLoop, this));
    }

    std::cout<<"KV store server running on port : "<<port<<"\n";

    return true;
};

bool TCPServer::stop() {

    if(shouldStop) {
        return true;
    }

    shouldStop = true;
    
    // Actively break the blocking accept() call before closing
    shutdown(serverFd, SHUT_RDWR);
    close(serverFd);
    
    clientCv.notify_all();
    
    if(acceptThread.joinable()){
        acceptThread.join();
    }
    for(auto& thread : workerThreads){
        if(thread.joinable()){
            thread.join();
        }
    }

    std::cout<<"\nKV store server stopped"<<"\n";

    return true;
};

TCPServer::TCPServer(const Config& config) { 
    
    port = config.port;
    stores.reserve(config.dbConfig.size());
    for(const auto& dbCfg : config.dbConfig){
        stores.emplace_back(std::make_unique<LRUStore>(dbCfg));
    }

    initCommandRegistry(); 
};

TCPServer::~TCPServer() {
    stop();
}