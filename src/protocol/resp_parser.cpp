#include "resp_parser.h"
#include <string>


bool RESPParser::parseBulkString(std::string& rawRequest, size_t& startIndex, RESPCommand& parsedRequest, int requestNo){
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

std::string RESPParser::serializeOK(){
    return "+OK" + CRLF;
}

std::string RESPParser::serializeError(const std::string& rawResponse){
    std::string formattedResponse = "-ERR ";
    formattedResponse += rawResponse;
    formattedResponse += CRLF;
    return formattedResponse;
}

std::string RESPParser::serializeBulk(const std::string& rawResponse) {
    std::string formattedResponse ="$";
    std::string noOfBytes = std::to_string(rawResponse.size());
    formattedResponse += noOfBytes;
    formattedResponse += CRLF;
    formattedResponse += rawResponse;
    formattedResponse += CRLF;
    return formattedResponse;
}

std::string RESPParser::serializeNullBulk(){
    return "$-1" + CRLF;
}


std::string RESPParser::serializeInteger(const std::string& rawResponse) {
    std::string formattedResponse =":";
    formattedResponse += rawResponse;
    formattedResponse += CRLF;
    return formattedResponse;
}

std::string RESPParser::serializeSimpleString(const std::string& rawResponse) {
    return "+" + rawResponse + CRLF;
}


RESPCommand RESPParser::parse(std::string& rawRequest){
    
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
        if(!parseBulkString(rawRequest, pos, parsedRequest, i)){
            return parsedRequest;
        }
    }
    
    return parsedRequest;
};

std::string RESPParser::serialize (ResponseType type, const std::string& rawResponse) {
    
    switch (type) {

        case ResponseType::OK : {
            return serializeOK();
        }

        case ResponseType::ERROR : {
            return serializeError(rawResponse);
        }

        case ResponseType::BULK : {
            return serializeBulk(rawResponse);
        }

        case ResponseType::NULLBULK : {
            return serializeNullBulk();
        }

        case ResponseType::INTEGER : {
            return serializeInteger(rawResponse);
        };

        case ResponseType::SIMPLE_STRING :{
            return serializeSimpleString(rawResponse);
        };
    }

    return "unknown response type";
}