#pragma once
#include "lru_store.h"
#include "resp_parser.h"
#include <asm-generic/socket.h>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>


class TCPServer {

    int serverFd;
    int port;
    std::vector<LRUStore>& stores;
    std::vector<std::thread> workerThreads;
    std::thread connectionThread;
    std::queue<int> clientFds;
    std::mutex queueMtx;
    std::condition_variable cvClientConnection;
    std::atomic<int> activeClients{0};
    std::atomic<bool> stopVariable{false};

    void handleCommand(const std::string& cmd, int dbIndex){
        // pending - still need to work on the RESP parser and then integrate it
    }

    void handleClient(int fd){
        
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
                break;;
            }

            buffer[bytesRead] = '\0';
            accumulated += std::string(buffer, bytesRead);
            

            RESPParser temp;
            RESPCommand parsedCmd = temp.parse(accumulated);
            
            // we got the full complete command
            if(parsedCmd.status == ParseStatus::OK){
                // call the method -> handleCommand()
                accumulated = "";
                continue;
            }
            
            // if the cmd is still to be revived from the network
            if(parsedCmd.status == ParseStatus::INCOMPLETE){
                continue;;
            }
            

            // else it means that we have got an error
            // handle the error -> produce the message according to the erroe type
                

            
        }
        

        // pending - still need to work on the RESP parser and then integrate it
    }
    

    void acceptLoop(){

        while(!stopVariable){
            sockaddr_in clientAddress;
            socklen_t clientLen = sizeof(clientAddress);
            int clientFd = accept(serverFd, (struct sockaddr*)&clientAddress, &clientLen);

            if (clientFd < 0) {
                std::cerr<<"Failed to connect to client : "<<strerror(errno)<<"\n";
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
            cvClientConnection.notify_one();
        }
    }

    void workerLoop() {

        while (!stopVariable) {
            std::unique_lock<std::mutex> lock(queueMtx);
            cvClientConnection.wait(lock, [this]{
                return !clientFds.empty() || stopVariable;
            });

            if(stopVariable){
                break;
            }

            int fd = clientFds.front();
            clientFds.pop();
            lock.unlock();

            handleClient(fd);
        }
    }


    public:

        TCPServer(std::vector<LRUStore>& _stores, int _port) : stores(_stores), port(_port) {};

        bool start(){

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
            connectionThread = std::thread(&TCPServer::acceptLoop, this);

            // create 10 worker thread -> 10 persistent connections
            workerThreads.reserve(10);
            for(int i = 0; i < 10; i++){
                workerThreads.emplace_back(std::thread(&TCPServer::workerLoop, this));
            }

            return true;
        };

        bool stop() {
            return true;
        };

};