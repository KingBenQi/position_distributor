#include "common.hpp"
#include <iostream>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <cstring>
#include <signal.h>
#include <fcntl.h>
#include <random>

class PositionClient {
private:
    int sockfd;
    std::string client_id;
    std::atomic<bool> running{true};
    std::atomic<bool> connected{false};
    
    std::map<std::string, double> position_cache;
    std::mutex cache_mutex;
    
    uint64_t last_received_seq;
    std::mutex seq_mutex;
    
    std::queue<std::string> outgoing_queue;
    std::mutex outgoing_mutex;
    std::condition_variable outgoing_cv;
    
    std::thread receive_thread;
    std::thread send_thread;
    std::thread ack_thread;
    
    std::mt19937 rng;
    std::uniform_real_distribution<double> position_dist;

public:
    PositionClient(const std::string& id) 
        : client_id(id), last_received_seq(0),
          position_dist(-1000.0, 1000.0) {
        
        std::random_device rd;
        rng.seed(rd());
        
        signal(SIGINT, [](int) { 
            std::cout << "Shutting down client...\n"; 
            exit(0); 
        });
        
        connectToServer();
        
        if (connected) {
            receive_thread = std::thread(&PositionClient::receiveLoop, this);
            send_thread = std::thread(&PositionClient::sendLoop, this);
            ack_thread = std::thread(&PositionClient::sendAcknowledgments, this);
        }
    }
    
    ~PositionClient() {
        running = false;
        
        outgoing_cv.notify_all();
        
        if (receive_thread.joinable()) {
            receive_thread.join();
        }
        
        if (send_thread.joinable()) {
            send_thread.join();
        }
        
        if (ack_thread.joinable()) {
            ack_thread.join();
        }
        
        if (connected) {
            close(sockfd);
        }
    }
    
    void run() {
        if (!connected) {
            std::cerr << "Not connected to server. Exiting.\n";
            return;
        }
        
        std::cout << "Client " << client_id << " running. Press Ctrl+C to exit.\n";
        
        std::vector<std::string> symbols = {
            "BTCUSDT", "ETHUSDT", "SOLUSDT", "AVAXUSDT", "DOGEUSDT"
        };
        
        while (running) {
            std::string symbol = symbols[std::rand() % symbols.size()] + "." + client_id;
            double position = position_dist(rng);
            
            updatePosition(symbol, position);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 + (std::rand() % 4000)));
        }
    }
    
    void updatePosition(const std::string& symbol, double position) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            position_cache[symbol] = position;
        }
        
        std::string msg;
        msg.reserve(128);
        
        std::ostringstream oss;
        oss << symbol << "|" << std::fixed << std::setprecision(8) << position << "\n";
        msg = oss.str();
        
        {
            std::lock_guard<std::mutex> lock(outgoing_mutex);
            outgoing_queue.push(msg);
        }
        
        outgoing_cv.notify_one();
        
        std::cout << "Updated position: " << symbol << " => " << position << std::endl;
    }

    
    std::map<std::string, double> getPositions() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        return position_cache;
    }

private:
    void connectToServer() {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("socket creation failed");
            return;
        }
        
        fcntl(sockfd, F_SETFL, O_NONBLOCK);

        // Increase socket buffer sizes for better throughput and reduced latency spikes
        int rcvbuf_size = 1024 * 1024; 
        int sndbuf_size = 1024 * 1024; 

        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
            perror("setsockopt SO_RCVBUF failed");
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size)) < 0) {
            perror("setsockopt SO_SNDBUF failed");
        }


        // Disable Nagle's algorithm for lower latency
        int nodelay_flag = 1;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay_flag, sizeof(nodelay_flag)) < 0) {
            perror("setsockopt TCP_NODELAY failed");
        }
                
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
        std::memset(server_addr.sin_zero, 0, sizeof(server_addr.sin_zero));
        
        bool connection_successful = false;
        int retry_count = 0;
        const int max_retries = 5;
        
        while (!connection_successful && retry_count < max_retries) {
            if (connect(sockfd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                if (errno == EINPROGRESS || errno == EALREADY) {
                    fd_set write_fds;
                    FD_ZERO(&write_fds);
                    FD_SET(sockfd, &write_fds);
                    
                    struct timeval timeout;
                    timeout.tv_sec = 1;
                    timeout.tv_usec = 0;
                    
                    int result = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
                    if (result > 0 && FD_ISSET(sockfd, &write_fds)) {
                        int error = 0;
                        socklen_t len = sizeof(error);
                        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                            perror("Connection failed");
                        } else {
                            connection_successful = true;
                        }
                    }
                } else if (errno != EINTR) {
                    perror("connect");
                    retry_count++;
                    std::cout << "Retrying connection (" << retry_count << "/" << max_retries << ")...\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY_MS));
                    
                    close(sockfd);
                    sockfd = socket(AF_INET, SOCK_STREAM, 0);
                    fcntl(sockfd, F_SETFL, O_NONBLOCK);
                }
            } else {
                connection_successful = true;
            }
        }
        
        if (!connection_successful) {
            std::cerr << "Failed to connect to server after " << max_retries << " attempts\n";
            close(sockfd);
            return;
        }
        
        std::cout << "Connected to server\n";
        connected = true;
        
        std::string hello_msg;
        hello_msg.reserve(64);
        hello_msg = "HELLO|" + client_id + "\n";
        send(sockfd, hello_msg.c_str(), hello_msg.size(), 0);
    }
    
    void receiveLoop() {
        char buffer[BUFFER_SIZE];
        std::string incoming_buffer;
        incoming_buffer.reserve(BUFFER_SIZE * 2);
        
        while (running && connected) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
            
            if (bytes > 0) {
                incoming_buffer.append(buffer, bytes);
                
                size_t pos = 0;
                size_t newline_pos;
                while ((newline_pos = incoming_buffer.find('\n', pos)) != std::string::npos) {
                    std::string message = incoming_buffer.substr(pos, newline_pos - pos);
                    
                    if (!message.empty()) {
                        processReceivedMessage(message);
                    }
                    
                    pos = newline_pos + 1;
                }
                
                if (pos > 0) {
                    incoming_buffer.erase(0, pos);
                }
            } else if (bytes == 0) {
                std::cout << "Server closed connection\n";
                connected = false;
                
                reconnect();
            } else if (bytes < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("recv");
                    connected = false;
                    
                    reconnect();
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    void sendLoop() {
        while (running) {
            std::string msg;
            bool has_message = false;
            
            {
                std::unique_lock<std::mutex> lock(outgoing_mutex);
                outgoing_cv.wait_for(lock, std::chrono::milliseconds(100),
                                    [this] { return !outgoing_queue.empty() || !running; });
                
                if (!running) {
                    break;
                }
                
                if (!outgoing_queue.empty()) {
                    msg = outgoing_queue.front();
                    outgoing_queue.pop();
                    has_message = true;
                }
            }
            
            if (has_message && connected) {
                ssize_t bytes_sent = send(sockfd, msg.c_str(), msg.size(), 0);
                
                if (bytes_sent < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("send");
                        connected = false;
                        
                        {
                            std::lock_guard<std::mutex> lock(outgoing_mutex);
                            outgoing_queue.push(msg);
                        }
                        
                        reconnect();
                    } else {
                        std::lock_guard<std::mutex> lock(outgoing_mutex);
                        outgoing_queue.push(msg);
                    }
                }
            }
        }
    }
    
    void sendAcknowledgments() {
        while (running && connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ACK_INTERVAL_MS));
            
            if (!connected) {
                continue;
            }
            
            Acknowledgment ack;
            ack.client_id = client_id;
            
            {
                std::lock_guard<std::mutex> lock(seq_mutex);
                ack.last_sequence_number = last_received_seq;
            }
            
            std::string serialized;
            serialized.reserve(64);
            serialized = ack.serializeForNetwork();
            
            send(sockfd, serialized.c_str(), serialized.size(), 0);
        }
    }
    
    void processReceivedMessage(const std::string& data) {
        try {
            Message msg = Message::deserialize(data);
            
            {
                std::lock_guard<std::mutex> lock(seq_mutex);
                if (msg.sequence_number <= last_received_seq) {
                    return;
                }
                
                last_received_seq = msg.sequence_number;
            }
            
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                position_cache[msg.position.symbol] = msg.position.net_position;
            }
            
            std::cout << "Received update from " << msg.source_id 
                      << ": " << msg.position.symbol 
                      << " => " << msg.position.net_position << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "Error processing received message: " << e.what() << std::endl;
            std::cerr << "Raw message data: \"" << data << "\"" << std::endl;
        }
    }
    
    void reconnect() {
        std::cout << "Attempting to reconnect...\n";
        
        close(sockfd);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY_MS));
        
        connectToServer();
        
        if (connected) {
            std::cout << "Reconnected to server\n";
        } else {
            std::cout << "Reconnection failed\n";
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <client_id>\n";
        return 1;
    }
    
    std::string client_id = argv[1];
    
    PositionClient client(client_id);
    client.run();
    
    return 0;
}