#pragma once
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <iomanip>

// Basic position structure 
struct SymbolPos {
    std::string symbol;
    double net_position;
};

struct Message {
    uint64_t sequence_number;    
    std::string source_id;       
    std::string timestamp;       
    SymbolPos position;          
    
    std::string serialize() const {
        std::ostringstream oss;
        oss << sequence_number << "|"
            << source_id << "|"
            << timestamp << "|"
            << position.symbol << "|"
            << std::fixed << std::setprecision(8) << position.net_position;
        return oss.str();
    }
    
    std::string serializeForLog() const {
        return serialize() + "\n";
    }
    
    std::string serializeForNetwork() const {
        return serialize() + "\n";
    }
    
    static Message deserialize(const std::string& s) {
        Message msg;
        std::istringstream iss(s);
        std::string token;
        
        if (std::getline(iss, token, '|')) {
            try {
                msg.sequence_number = std::stoull(token);
            } catch (const std::exception& e) {
                throw std::runtime_error("Invalid sequence number format: " + token);
            }
        } else {
            throw std::runtime_error("Missing sequence number in message");
        }
        
        if (std::getline(iss, token, '|')) {
            msg.source_id = token;
        } else {
            throw std::runtime_error("Missing source_id in message");
        }
        
        if (std::getline(iss, token, '|')) {
            msg.timestamp = token;
        } else {
            throw std::runtime_error("Missing timestamp in message");
        }
        
        if (std::getline(iss, token, '|')) {
            msg.position.symbol = token;
        } else {
            throw std::runtime_error("Missing symbol in message");
        }
        
        if (std::getline(iss, token)) {
            try {
                msg.position.net_position = std::stod(token);
            } catch (const std::exception& e) {
                throw std::runtime_error("Invalid position value format: " + token);
            }
        } else {
            throw std::runtime_error("Missing position value in message");
        }
        
        return msg;
    }

    static std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        auto epoch = now_ms.time_since_epoch();
        auto value = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
        return std::to_string(value);
    }
};

struct Acknowledgment {
    std::string client_id;
    uint64_t last_sequence_number;
    
    std::string serialize() const {
        std::ostringstream oss;
        oss << "ACK|" << client_id << "|" << last_sequence_number;
        return oss.str();
    }
    
    std::string serializeForNetwork() const {
        return serialize() + "\n";
    }
    
    static Acknowledgment deserialize(const std::string& s) {
        Acknowledgment ack;
        std::istringstream iss(s);
        std::string token;
        
        std::getline(iss, token, '|');
        
        if (std::getline(iss, token, '|')) {
            ack.client_id = token;
        } else {
            throw std::runtime_error("Missing client_id in acknowledgment");
        }
        
        if (std::getline(iss, token)) {
            try {
                ack.last_sequence_number = std::stoull(token);
            } catch (const std::exception& e) {
                throw std::runtime_error("Invalid sequence number in acknowledgment: " + token);
            }
        } else {
            throw std::runtime_error("Missing sequence number in acknowledgment");
        }
        
        return ack;
    }
};

constexpr int SERVER_PORT = 9000;
constexpr const char* SERVER_IP = "127.0.0.1";
constexpr int RECONNECT_DELAY_MS = 1000;
constexpr int BUFFER_SIZE = 4096; 
constexpr int ACK_INTERVAL_MS = 500;  

constexpr const char* MSG_TYPE_POSITION = "POS";
constexpr const char* MSG_TYPE_ACK = "ACK";
constexpr const char* MSG_TYPE_HELLO = "HELLO";