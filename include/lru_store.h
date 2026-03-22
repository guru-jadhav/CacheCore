#pragma once
// #pragma once tells the compiler — "no matter how many times you encounter this file, only process it once."
// #pragma once must be the absolute first line of the file. If another file includes this header before the guard 
// is registered, double inclusion can still happen. Move it to line 1.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <queue>
#include <unordered_map>
#include <string>
#include <optional>
#include <vector>


struct TTLNode {
    std::string key;
    std::chrono::steady_clock::time_point expTime;

    bool operator>(const TTLNode& other) const {
        return expTime > other.expTime;
    }

    TTLNode(const std::string& _key, const std::chrono::steady_clock::time_point& _expTime) : key(_key), expTime(_expTime) {}
};


struct Node {
    std::string key;
    std::string value;
    std::optional<std::chrono::steady_clock::time_point> expTime = std::nullopt;
    Node* prev;
    Node* next;

    Node(const std::string& _key, const std::string& _value) : key(_key), value(_value), prev(nullptr), next(nullptr) {}

};

class LRUStore {

    void removeNode(Node* curr);

    void moveToHead(Node* curr);

    void linkToHead(Node* curr);

    void deleteNode(Node* toDelete);

    void clearStore();

    bool isExpired(Node* curr);

    void addExpTime(Node* curr, const bool isExpires, const size_t duration = 0);

    void addToTTLHeap(const std::string& key, std::chrono::steady_clock::time_point _expTime);

    void removeExpKeys();

    std::unordered_map<std::string, Node*> store;
    Node* dummyHead;
    Node* dummyTail;
    // store.size() returns size_t which is unsigned. maxCapacity is int which is signed. 
    // This will trigger a compiler warning and can behave unexpectedly if maxCapacity is somehow negative.
    const size_t maxCapacity;
    const size_t ttl; // 60s by default
    std::priority_queue<TTLNode, std::vector<TTLNode>, std::greater<TTLNode>> tllHeap;

    public:


        //  ttl(std::max(ttl, _ttl) this is wrong we are reading ttl even before it's initialized it
        //  ttl(std::max((size_t)60, _ttl))
        LRUStore(size_t _maxCapacity, size_t _ttl) : maxCapacity(_maxCapacity), ttl(std::max((size_t)60, _ttl)){
            dummyHead = new Node("-1", "-1");
            dummyTail = new Node("-1", "-1");

            dummyHead->next = dummyTail;
            dummyTail->prev = dummyHead;
        }

        ~LRUStore(){
            Node* curr = dummyHead;
            while(curr){
                Node* nextNode = curr->next;
                delete curr;
                curr = nextNode;
            }

            while(!tllHeap.empty()){
                tllHeap.pop();
            }
        }
        
        std::optional<std::string> GET(const std::string& _key);

        bool SET(const std::string& _key, const std::string& _value, const bool isExpires = true);

        // we can make the DEL as void -> if exists we delete it we just return
        void DEL(const std::string& _key);

        bool EXISTS(const std::string& _key);

        void CLEAR();

        void EXPIRE(const std::string& _key, const size_t duration);
};


/*
    - GET check if key exists -> if yes -> check if the key is expired
    but to do so we need a expity time to be stored in the Node - else we won't 
    be able to determine the exp time if we have exp time inte store node then we can do lazy deletion
    on the tllheap else we have to pop the heap till we get expt time which is greater then top

    - SET -> we just update the latest exp time and push it in the tll heap, but this will cause
    duplicates and this will delete some key x which has exptime but it was also updated and pushed
    so it should not be deleted

    - so to tackel this we need access to all the heap nodes -> use unorder_map
    and when get and set comes we use the map and directly access the node and check and set the exptime
    and process according to it

    - one this is done we will bind this up in function and for every obj of class LRUStore we will
    scheduel a thread every x interval and call this function

*/

/*

1 - SET with isExpires flag - by default true (can add default parameter or we can get a new SET() with new signature)
    if user sets isExpires as flase then the key never expires - no evictions
2 - EXPIRES () -  will explicity set the expTime for a specific key - max(store default ttl, userinput)
3 - change the ttl time to seconds -> more flexible 
*/