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
    NULLBULK,
    SIMPLE_STRING
};

const std::string CRLF = "\r\n";

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

    bool parseBulkString(std::string& rawRequest, size_t& startIndex, RESPCommand& parsedRequest, int requestNo);
    std::string serializeOK();
    std::string serializeError(const std::string& rawResponse);
    std::string serializeBulk(const std::string& rawResponse);
    std::string serializeNullBulk();
    std::string serializeInteger(const std::string& rawResponse);
    std::string serializeSimpleString(const std::string& rawResponse);

    public:
        RESPCommand parse(std::string& rawRequest);
        std::string serialize (ResponseType type, const std::string& rawResponse);
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

    Note: This is a custom RESP implementation. Unlike standard Redis, CacheCore 
    requires the Database Index to be passed as the first argument on every call 
    to enable stateless routing.

    1 single request will be something like : 

    no. of bulk string, db index, command, key, value, flags, etc.

    ResponseType = 
        +OK\r\n          — simple string (SET, DEL, EXPIRE success)
        -ERR msg\r\n     — error
        $3\r\nfoo\r\n    — bulk string (GET value)
        $-1\r\n          — null bulk string (GET miss)
        :1\r\n           — integer (EXISTS)

*/