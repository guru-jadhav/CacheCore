#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ParseErr {
    const std::string EMPTY_REQUEST         = "empty request";
    const std::string EXPECTED_ARRAY        = "expected '*' at start of RESP array";
    const std::string INVALID_ARRAY_COUNT   = "non-numeric character in array count after '*'";
    const std::string MISSING_CRLF          = "expected \\r\\n terminator not found";
    const std::string EXPECTED_BULK         = "expected '$' at start of bulk string";
    const std::string INVALID_BULK_LEN      = "non-numeric character in bulk string length after '$'";
    const std::string BULK_DATA_SHORT       = "bulk string data shorter than declared length";
    const std::string INVALID_DB_INDEX      = "db index must be a valid number";
}

enum class ParseStatus {
    OK,
    INVALID_FORMAT,
    INVALID_DB_INDEX,
    UNKNOWN_COMMAND,
    INCOMPLETE
};

enum class ResponseType {
    OK,
    ERROR,
    BULK,
    INTEGER,
    NULLBULK
};

const std::string terminal = "\r\n";

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
            parsedRequest.errorMsg = ParseErr::EXPECTED_BULK;
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
                parsedRequest.errorMsg = ParseErr::INVALID_BULK_LEN;
                return false;
            }
        }

        if(index == rawRequest.size()){
            parsedRequest.status = ParseStatus::INCOMPLETE;
            return false;
        }

        if(rawRequest[index + 1] != '\n'){
            parsedRequest.status = ParseStatus::INVALID_FORMAT;
            parsedRequest.errorMsg = ParseErr::MISSING_CRLF;
            return false;
        }
        
        index += 2;;

        
        size_t dataLen = std::stoi(dataLenStr);
        std::string data = rawRequest.substr(index, dataLen);
        if(data.size() < dataLen){
            parsedRequest.status = ParseStatus::INCOMPLETE;
            parsedRequest.errorMsg = ParseErr::BULK_DATA_SHORT;
            return false;
        }
        

        index += dataLen;
        
        if(index + 1 >= rawRequest.size()){
            parsedRequest.status = ParseStatus::INCOMPLETE;
            return false;
        }

        if(rawRequest[index] != '\r'){
            parsedRequest.status = ParseStatus::INVALID_FORMAT;
            parsedRequest.errorMsg = ParseErr::MISSING_CRLF;
            return false;
        }

        index++;
        if(rawRequest[index] != '\n'){
            parsedRequest.status = ParseStatus::INVALID_FORMAT;
            parsedRequest.errorMsg = ParseErr::MISSING_CRLF;
            return false;
        }
        
        
        index++;
        startIndex = index;

        if(requestNo == 0){
            try {
                
                /*
                    what if the DB index bulk string is not a number like 
                    -> "abc" 
                    -> "" or an empty string
                */
                parsedRequest.dbIndex = std::stoi((data));
            } catch (...) {
                parsedRequest.status =  ParseStatus::INVALID_FORMAT;
                parsedRequest.errorMsg = ParseErr::INVALID_DB_INDEX;
                return false;
            }
        }else if(requestNo == 1){
            parsedRequest.command = data;
        }else{
            parsedRequest.args.emplace_back(data);
        }
        

        return true;
    }

    std::string simpleStringResponse(){
        return "+OK" + terminal;
    }

    std::string errorResponse(const std::string& rawResponse){
        std::string formattedResponse = "-ERR ";
        formattedResponse += rawResponse;
        formattedResponse += terminal;
        return formattedResponse;
    }

    std::string bulkResponse(const std::string& rawResponse) {
        std::string formattedResponse ="$";
        std::string noOfBytes = std::to_string(rawResponse.size());
        formattedResponse += noOfBytes;
        formattedResponse += terminal;
        formattedResponse += rawResponse;
        formattedResponse += terminal;
        return formattedResponse;
    }

    std::string nullBulkResponse(){
        return "$-1" + terminal;
    }

    std::string integerResponse(const std::string& rawResponse) {
        std::string formattedResponse =":";
        formattedResponse += rawResponse;
        formattedResponse += terminal;
        return formattedResponse;
    }

    public:

        RESPCommand parse(std::string& rawRequest){
            
            RESPCommand parsedRequest;
            
            if(rawRequest.size() <= 0){
                parsedRequest.status = ParseStatus::INCOMPLETE;
                parsedRequest.errorMsg = ParseErr::EMPTY_REQUEST;
                return parsedRequest;
            }
            
            if(rawRequest[0] != '*'){
                parsedRequest.status = ParseStatus::INVALID_FORMAT;
                parsedRequest.errorMsg = ParseErr::EXPECTED_ARRAY;
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
                    parsedRequest.errorMsg = ParseErr::INVALID_ARRAY_COUNT;
                    return parsedRequest;
                }
            }
            
            if(pos == rawRequest.size()){
                parsedRequest.status = ParseStatus::INCOMPLETE;
                return parsedRequest;
            }
            
            
            if(rawRequest[pos + 1] != '\n'){
                parsedRequest.status = ParseStatus::INVALID_FORMAT;
                parsedRequest.errorMsg = ParseErr::MISSING_CRLF;
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

        std::string serialize (ResponseType type, const std::string& rawResponse) {
            
            switch (type) {

                case ResponseType::OK : {
                    return simpleStringResponse();
                }

                case ResponseType::ERROR : {
                    return errorResponse(rawResponse);
                }

                case ResponseType::BULK : {
                    return bulkResponse(rawResponse);
                }

                case ResponseType::NULLBULK : {
                    return nullBulkResponse();
                }

                case ResponseType::INTEGER : {
                    return integerResponse(rawResponse);
                };
            }

            return "unknown response type";
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

    ResponseType = 
        +OK\r\n          — simple string (SET, DEL, EXPIRE success)
        -ERR msg\r\n     — error
        $3\r\nfoo\r\n    — bulk string (GET value)
        $-1\r\n          — null bulk string (GET miss)
        :1\r\n           — integer (EXISTS)

*/