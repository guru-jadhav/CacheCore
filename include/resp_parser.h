#pragma once
#include <cstddef>
#include <string>
#include <vector>

enum class ParseStatus {
    OK,
    INVALID_FORMAT,
    INVALID_DB_INDEX,
    UNKNOWN_COMMAND,
    INCOMPLETE
};


struct RESPCommand {
    int dbIndex;
    ParseStatus status = ParseStatus::OK;
    std::string errorMsg;
    std::string command;
    std::vector<std::string> args;

    RESPCommand(){};

    RESPCommand(int _dbIndex, std::string _errorMsg, std::string _command, std::vector<std::string> _args) : 
        dbIndex(_dbIndex),
        errorMsg(_errorMsg),
        command(_command),
        args(_args) {};
        
};

class RESPParser{

    bool helper(std::string& rawRequest, size_t& startIndex, RESPCommand& parsedRequest, int requestNo){
        if(startIndex >= rawRequest.size()){
            parsedRequest.status = ParseStatus::INCOMPLETE;
            return false;
        }

        if(rawRequest[startIndex] != '$'){
            parsedRequest.status = ParseStatus::INVALID_FORMAT;
            return false;
        }

        std::string dataLenStr;
        int index = startIndex + 1;

        while(index < rawRequest.size() && rawRequest[index] != '\r'){
            if(rawRequest[index] >= '0' && rawRequest[index] <= '9'){
                dataLenStr += rawRequest[index];
                index++;
            }else{
                parsedRequest.status = ParseStatus::INVALID_FORMAT;
                return false;
            }
        }

        if(index == rawRequest.size()){
            parsedRequest.status = ParseStatus::INCOMPLETE;
            return false;
        }

        if(rawRequest[index + 1] != '\n'){
            parsedRequest.status = ParseStatus::INVALID_FORMAT;
            return false;
        }
        
        index += 2;;

        
        size_t dataLen = std::stoi(dataLenStr);
        std::string data = rawRequest.substr(index, dataLen);
        if(data.size() < dataLen){
            parsedRequest.status = ParseStatus::INCOMPLETE;
            return false;
        }
        

        index += dataLen;
        
        if(index + 1 >= rawRequest.size()){
            parsedRequest.status = ParseStatus::INCOMPLETE;
            return false;
        }

        index++;
        if(rawRequest[index] != '\r'){
            parsedRequest.status = ParseStatus::INVALID_FORMAT;
            return false;
        }

        index++;
        if(rawRequest[index] != '\n'){
            parsedRequest.status = ParseStatus::INVALID_FORMAT;
            return false;
        }
        
        
        index++;
        startIndex = index;

        if(requestNo == 0){
            parsedRequest.dbIndex = std::stoi((data));
        }else if(requestNo == 1){
            parsedRequest.command = data;
        }else{
            parsedRequest.args.emplace_back(data);
        }
        

        return true;
    }


    public:

        RESPCommand parse(std::string& rawRequest){
            
            RESPCommand parsedRequest;
            
            if(rawRequest.size() <= 0){
                parsedRequest.status = ParseStatus::INCOMPLETE;
                return parsedRequest;
            }
            
            if(rawRequest[0] != '*'){
                parsedRequest.status = ParseStatus::INVALID_FORMAT;
                return parsedRequest;
            }

            
            size_t pos = 1;
            std::string noOfBulkStreams;
            while(pos < rawRequest.size() && rawRequest[pos] != '\r'){
                // only numbers allowed
                if(rawRequest[pos] >= '0' && rawRequest[pos] <= '9'){
                    noOfBulkStreams += rawRequest[pos];
                    pos++;
                }else{
                    parsedRequest.status = ParseStatus::INVALID_FORMAT;
                    return parsedRequest;
                }
            }
            
            if(pos == rawRequest.size()){
                parsedRequest.status = ParseStatus::INCOMPLETE;
                return parsedRequest;
            }
            
            
            if(rawRequest[pos + 1] != '\n'){
                parsedRequest.status = ParseStatus::INVALID_FORMAT;
                return parsedRequest;
            }
            
            pos += 2;
            int N = std::stoi((noOfBulkStreams));

            for(int i = 0; i < N; i++){
                if(!helper(rawRequest, pos, parsedRequest, i)){
                    return parsedRequest;
                }
            }
            
            return parsedRequest;
        };

        std::string serialize (std::string& rawResponse) {
            std::string formattedRequest;
            
            // format the request according to the RESP protocol;

            return formattedRequest;
        }
};

/*

They are mostly used in responses, not requests:
    1 - +OK\r\n → success
    2 - :1\r\n → integer (like count)
    3 - -ERR ... → error


Client usually sends array of bulk streams:

    1 - *no of bulk streams
    2 - $no of chars in the bulk bulk stream

    *2\r\n$3\r\nGET\r\n$3\r\nkey\r\n
    *2\r\n$3\r\nGET\r\n$3\r\nabc\r\n
    *2\r\n$3\r\nGET\r\n$3\r\nID1\r\n

    For V1 - we will not support pipelining like redis does

    1 single request will be something like : 

    no. of bulk string, db index, command, key, value, flags, etc.

*/