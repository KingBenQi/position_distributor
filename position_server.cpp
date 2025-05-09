#include "common.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <queue>
#include <condition_variable>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <signal.h>
#include <atomic>
#include <fstream>

struct ClientInfo {
    int socket_fd;
    std::string client_id;
    uint64_t last_ack_seq;
    std::chrono::steady_clock::time_point last_activity;
};

class MessageQueue {
private:
    std::queue<Message> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> should_stop{false};

public:
    void push(const Message& msg) {
        std::unique_lock<std::mutex> lock(mutex);
        queue.push(msg);
        cv.notify_one();
    }

    bool pop(Message& msg, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex);
        if (queue.empty() && timeout_ms > 0) {
            auto status = cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), 
                                     [this] { return !queue.empty() || should_stop; });
            if (!status || should_stop) {
                return false;
            }
        }
        
        if (queue.empty()) {
            return false;
        }
        
        msg = queue.front();
        queue.pop();
        return true;
    }

    void stop() {
        should_stop = true;
        cv.notify_all();
    }
};

class PositionServer {
private:
    int server_fd;
    std::vector<ClientInfo> clients;
    std::mutex clients_mutex;
    
    uint64_t global_sequence_number;
    std::map<std::string, uint64_t> client_sequence_numbers;
    std::map<uint64_t, Message> message_history;
    std::mutex history_mutex;
    MessageQueue message_queue;
    
    std::string log_file = "position_server.log";
    std::mutex log_mutex;
    
    std::atomic<bool> running{true};
    
    std::thread processor_thread;
    std::thread health_check_thread;

public:
    PositionServer() : global_sequence_number(0) {
        signal(SIGINT, [](int) { 
            std::cout << "Shutting down server...\n"; 
            exit(0); 
        });
        
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }
    
        // Increase socket buffer sizes for better throughput and reduced latency spikes
        int rcvbuf_size = 1024 * 1024; 
        int sndbuf_size = 1024 * 1024; 

        if (setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
            perror("setsockopt SO_RCVBUF failed");
        }

        if (setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size)) < 0) {
            perror("setsockopt SO_SNDBUF failed");
        }
        
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt SO_REUSEADDR failed");
            exit(EXIT_FAILURE);
        }
        
        if (setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
            perror("setsockopt TCP_NODELAY failed");
            exit(EXIT_FAILURE);
        }
        
        fcntl(server_fd, F_SETFL, O_NONBLOCK);
        
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(SERVER_PORT);
        addr.sin_addr.s_addr = INADDR_ANY;
        std::memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
        
        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind failed");
            exit(EXIT_FAILURE);
        }
        
        if (listen(server_fd, 10) < 0) {
            perror("listen failed");
            exit(EXIT_FAILURE);
        }
        
        recoverFromLog();
        
        processor_thread = std::thread(&PositionServer::processMessages, this);
        
        health_check_thread = std::thread(&PositionServer::checkClientHealth, this);
    }
    
    ~PositionServer() {
        running = false;
        message_queue.stop();
        
        if (processor_thread.joinable()) {
            processor_thread.join();
        }
        
        if (health_check_thread.joinable()) {
            health_check_thread.join();
        }
        
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const auto& client : clients) {
            close(client.socket_fd);
        }
        
        close(server_fd);
    }
    
    void run() {
        std::cout << "Position Server running on port " << SERVER_PORT << "...\n";
        
        while (running) {
            acceptNewConnections();
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
        }
    }

private:
    void acceptNewConnections() {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept failed");
            }
            return;
        }
        
        fcntl(client_fd, F_SETFL, O_NONBLOCK);
        int client_rcvbuf_size = 1024 * 1024;
        int client_sndbuf_size = 1024 * 1024; 

        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &client_rcvbuf_size, sizeof(client_rcvbuf_size)) < 0) {
            perror("setsockopt SO_RCVBUF for client failed");
        }

        if (setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &client_sndbuf_size, sizeof(client_sndbuf_size)) < 0) {
            perror("setsockopt SO_SNDBUF for client failed");
        }
        // Disable Nagle's algorithm for client socket
        int nodelay_flag = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay_flag, sizeof(nodelay_flag)) < 0) {
            perror("setsockopt TCP_NODELAY for client failed");
        }        
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "New connection from " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;
        
        std::string temp_id = "temp_" + std::to_string(client_fd);
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back({client_fd, temp_id, 0, std::chrono::steady_clock::now()});
        }
        
        std::thread(&PositionServer::handleClient, this, client_fd).detach();
    }
    
    void handleClient(int client_fd) {
        char buffer[BUFFER_SIZE];
        std::string client_id;
        
        while (running) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
            
            if (bytes > 0) {
                std::string msg(buffer, bytes);
                if (msg.substr(0, 5) == "HELLO") {
                    size_t separator = msg.find('|');
                    if (separator != std::string::npos) {
                        client_id = msg.substr(separator + 1);
                        size_t newline = client_id.find('\n');
                        if (newline != std::string::npos) {
                            client_id = client_id.substr(0, newline);
                        }
                        break;
                    }
                }
                
            } else if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                closeClient(client_fd);
                return;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        bool is_reconnection = false;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            
            auto it = clients.begin();
            while (it != clients.end()) {
                if (it->socket_fd == client_fd && it->client_id.substr(0, 5) == "temp_") {
                    it = clients.erase(it);
                } else {
                    ++it;
                }
            }
            
            auto existing_client = std::find_if(clients.begin(), clients.end(),
                                              [&client_id](const ClientInfo& client) {
                                                  return client.client_id == client_id;
                                              });
            
            if (existing_client != clients.end()) {
                is_reconnection = true;
                
                if (existing_client->socket_fd != -1 && existing_client->socket_fd != client_fd) {
                    close(existing_client->socket_fd);
                }
                
                existing_client->socket_fd = client_fd;
                existing_client->last_activity = std::chrono::steady_clock::now();
            } else {
                clients.push_back({client_fd, client_id, 0, std::chrono::steady_clock::now()});
            }
        }
        
        if (is_reconnection) {
            std::cout << "Client " << client_id << " reconnected\n";
        } else {
            std::cout << "Client " << client_id << " registered\n";
        }
        
        sendHistoricalMessages(client_fd, client_id);
        
        std::string incoming_buffer;
        while (running) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
            
            if (bytes > 0) {
                incoming_buffer.append(buffer, bytes);
                
                size_t pos = 0;
                size_t newline_pos;
                while ((newline_pos = incoming_buffer.find('\n', pos)) != std::string::npos) {
                    std::string message = incoming_buffer.substr(pos, newline_pos - pos);
                    
                    if (!message.empty()) {
                        processClientData(client_fd, client_id, message);
                    }
                    
                    pos = newline_pos + 1;
                }
                
                if (pos > 0) {
                    incoming_buffer.erase(0, pos);
                }
                
                std::lock_guard<std::mutex> lock(clients_mutex);
                for (auto& client : clients) {
                    if (client.socket_fd == client_fd) {
                        client.last_activity = std::chrono::steady_clock::now();
                        break;
                    }
                }
            } else if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                std::cout << "Client " << client_id << " disconnected\n";
                closeClient(client_fd);
                return;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    void processClientData(int client_fd, const std::string& client_id, const std::string& data) {
        (void)client_fd;
        
        if (data.substr(0, 3) == "ACK") {
            try {
                Acknowledgment ack = Acknowledgment::deserialize(data);
                updateClientAck(client_id, ack.last_sequence_number);
            } catch (const std::exception& e) {
                std::cerr << "Error processing ACK from " << client_id << ": " << e.what() << std::endl;
            }
            return;
        }
        
        SymbolPos pos;
        try {
            size_t separator = data.find('|');
            if (separator != std::string::npos) {
                pos.symbol = data.substr(0, separator);
                pos.net_position = std::stod(data.substr(separator + 1));
            } else {
                throw std::runtime_error("Invalid message format");
            }
            
            Message msg;
            msg.position = pos;
            msg.source_id = client_id;
            msg.timestamp = Message::getCurrentTimestamp();
            
            {
                std::lock_guard<std::mutex> lock(history_mutex);
                msg.sequence_number = ++global_sequence_number;
                
                message_history[msg.sequence_number] = msg;
                
                logMessage(msg);
                std::cout << "Logged message: " << msg.sequence_number << "|" << client_id 
                << "|" << msg.timestamp << "|" << pos.symbol << "|" << pos.net_position << std::endl;
            }
            
            message_queue.push(msg);
            
        } catch (const std::exception& e) {
            std::cerr << "Error processing message from " << client_id << ": " << e.what() << std::endl;
            std::cerr << "Raw message data: \"" << data << "\"" << std::endl;
        }
    }
    
    void processMessages() {
        while (running) {
            Message msg;
            if (message_queue.pop(msg, 100)) {
                broadcastMessage(msg);
            }
        }
    }
    
    void broadcastMessage(const Message& msg) {
        std::string serialized;
        serialized.reserve(256); 
        serialized = msg.serializeForNetwork();
        
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const auto& client : clients) {
            if (client.client_id == msg.source_id || client.socket_fd == -1) {
                continue;
            }
            
            send(client.socket_fd, serialized.c_str(), serialized.size(), 0);
        }
    }
    
    void closeClient(int client_fd) {
        std::lock_guard<std::mutex> lock(clients_mutex);
        
        auto it = std::find_if(clients.begin(), clients.end(),
                              [client_fd](const ClientInfo& client) {
                                  return client.socket_fd == client_fd;
                              });
        
        if (it != clients.end()) {
            close(client_fd);
            it->socket_fd = -1;
        } else {
            close(client_fd);
        }
    }
    
    void updateClientAck(const std::string& client_id, uint64_t seq_num) {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto& client : clients) {
            if (client.client_id == client_id) {
                client.last_ack_seq = seq_num;
                break;
            }
        }
    }
    
    void sendHistoricalMessages(int client_fd, const std::string& client_id) {
        std::lock_guard<std::mutex> lock(history_mutex);
        
        uint64_t start_seq = 0;
        {
            std::lock_guard<std::mutex> clients_lock(clients_mutex);
            for (const auto& client : clients) {
                if (client.client_id == client_id) {
                    start_seq = client.last_ack_seq;
                    break;
                }
            }
        }
        std::cout << "Sending historical messages to " << client_id << " (from seq " << start_seq << ")" << std::endl;

        int message_count = 0;
        std::string serialized;
        serialized.reserve(256);
        for (const auto& [seq, msg] : message_history) {
            if (seq > start_seq &&  msg.source_id != client_id){
                serialized = msg.serializeForNetwork();
                send(client_fd, serialized.c_str(), serialized.size(), 0);
                message_count++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        
        std::cout << "Sent " << message_count << " historical messages to " << client_id << std::endl;
    
    }
    
    void logMessage(const Message& msg) {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::ofstream log(log_file, std::ios::app);
        if (log.is_open()) {
            std::string logMsg;
            logMsg.reserve(256);
            logMsg = msg.serializeForLog();
            log << logMsg;
        }
    }
    
    void recoverFromLog() {
        std::ifstream log(log_file);
        if (!log.is_open()) {
            std::cout << "No log file found for recovery\n";
            return;
        }
        
        int recovered_count = 0;
        std::string line;
        while (std::getline(log, line)) {
            try {
                Message msg = Message::deserialize(line);
                
                std::lock_guard<std::mutex> lock(history_mutex);
                message_history[msg.sequence_number] = msg;
                
                if (msg.sequence_number > global_sequence_number) {
                    global_sequence_number = msg.sequence_number;
                }
                
                recovered_count++;
            } catch (const std::exception& e) {
                std::cerr << "Error recovering message from log: " << e.what() << std::endl;
            }
        }
        
        std::cout << "Recovered " << recovered_count << " messages, global sequence now at "
                  << global_sequence_number << std::endl;
    }
    
    void checkClientHealth() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            std::vector<int> disconnected_clients;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                auto now = std::chrono::steady_clock::now();
                
                for (const auto& client : clients) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - client.last_activity).count();
                    
                    if (elapsed > 30) {
                        disconnected_clients.push_back(client.socket_fd);
                    }
                }
            }
            
            for (int fd : disconnected_clients) {
                std::cout << "Client health check: closing inactive client\n";
                closeClient(fd);
            }
        }
    }
};

int main() {
    PositionServer server;
    server.run();
    return 0;
}