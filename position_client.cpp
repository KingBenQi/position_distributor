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
    
    // Position tracking
    std::map<std::string, double> position_cache;
    std::mutex cache_mutex;
    
    // Message sequence tracking for order preservation
    uint64_t last_received_seq;
    uint64_t local_sequence_number;
    std::mutex seq_mutex;
    
    // Message queues for reliable delivery
    std::queue<std::string> outgoing_queue;
    std::mutex outgoing_mutex;
    std::condition_variable outgoing_cv;
    
    // Worker threads
    std::thread receive_thread;
    std::thread send_thread;
    std::thread ack_thread;
    
    // Random generator for simulated positions
    std::mt19937 rng;
    std::uniform_real_distribution<double> position_dist;

public:
    PositionClient(const std::string& id) 
        : client_id(id), last_received_seq(0), local_sequence_number(0),
          position_dist(-1000.0, 1000.0) {
        
        // Seed random generator
        std::random_device rd;
        rng.seed(rd());
        
        // Set up signal handlers
        signal(SIGINT, [](int) { 
            std::cout << "Shutting down client...\n"; 
            exit(0); 
        });
        
        // Connect to server
        connectToServer();
        
        // Start worker threads
        if (connected) {
            receive_thread = std::thread(&PositionClient::receiveLoop, this);
            send_thread = std::thread(&PositionClient::sendLoop, this);
            ack_thread = std::thread(&PositionClient::sendAcknowledgments, this);
        }
    }
    
    ~PositionClient() {
        running = false;
        
        // Notify threads to exit
        outgoing_cv.notify_all();
        
        // Join threads
        if (receive_thread.joinable()) {
            receive_thread.join();
        }
        
        if (send_thread.joinable()) {
            send_thread.join();
        }
        
        if (ack_thread.joinable()) {
            ack_thread.join();
        }
        
        // Close socket
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
        
        // Simulate trading activity by generating random positions
        std::vector<std::string> symbols = {
            "BTCUSDT", "ETHUSDT", "SOLUSDT", "AVAXUSDT", "DOGEUSDT"
        };
        
        while (running) {
            // Generate a random position update every few seconds
            std::string symbol = symbols[std::rand() % symbols.size()] + "." + client_id;
            double position = position_dist(rng);
            
            // Create position update
            updatePosition(symbol, position);
            
            // Sleep random interval between 1-5 seconds
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 + (std::rand() % 4000)));
        }
    }
    
    // Update a trading position and broadcast to other clients
    void updatePosition(const std::string& symbol, double position) {
        // Update local cache
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            position_cache[symbol] = position;
        }
        
        // Format message as simple "symbol|position" to be sequenced by server
        std::ostringstream oss;
        oss << symbol << "|" << std::fixed << std::setprecision(8) << position << "\n";
        std::string msg = oss.str();
        
        // Queue for sending
        {
            std::lock_guard<std::mutex> lock(outgoing_mutex);
            outgoing_queue.push(msg);
        }
        
        // Notify sender thread
        outgoing_cv.notify_one();
        
        // Log the position update
        std::cout << "Updated position: " << symbol << " => " << position << std::endl;
    }
    
    // Get current positions for all symbols
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
        
        // Set socket to non-blocking for better performance
        fcntl(sockfd, F_SETFL, O_NONBLOCK);
        
        // Prepare server address
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
        std::memset(server_addr.sin_zero, 0, sizeof(server_addr.sin_zero));
        
        // Connect with retry
        bool connection_successful = false;
        int retry_count = 0;
        const int max_retries = 5;
        
        while (!connection_successful && retry_count < max_retries) {
            if (connect(sockfd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                if (errno == EINPROGRESS || errno == EALREADY) {
                    // Connection in progress, wait and check status
                    fd_set write_fds;
                    FD_ZERO(&write_fds);
                    FD_SET(sockfd, &write_fds);
                    
                    struct timeval timeout;
                    timeout.tv_sec = 1;
                    timeout.tv_usec = 0;
                    
                    int result = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
                    if (result > 0 && FD_ISSET(sockfd, &write_fds)) {
                        // Check for actual connection success
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
                    
                    // Create new socket
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
        
        // Send HELLO message with client ID
        std::string hello_msg = "HELLO|" + client_id + "\n";
        send(sockfd, hello_msg.c_str(), hello_msg.size(), 0);
    }
    
    void receiveLoop() {
        char buffer[BUFFER_SIZE];
        std::string incoming_buffer;
        
        while (running && connected) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
            
            if (bytes > 0) {
                // Append received data to any incomplete message from previous read
                incoming_buffer.append(buffer, bytes);
                
                // Process complete messages
                size_t pos = 0;
                size_t newline_pos;
                while ((newline_pos = incoming_buffer.find('\n', pos)) != std::string::npos) {
                    // Extract a complete message
                    std::string message = incoming_buffer.substr(pos, newline_pos - pos);
                    
                    // Process the message
                    if (!message.empty()) {
                        processReceivedMessage(message);
                    }
                    
                    // Move past this message
                    pos = newline_pos + 1;
                }
                
                // Remove processed messages from the buffer
                if (pos > 0) {
                    incoming_buffer.erase(0, pos);
                }
            } else if (bytes == 0) {
                // Server closed connection
                std::cout << "Server closed connection\n";
                connected = false;
                
                // Try to reconnect
                reconnect();
            } else if (bytes < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("recv");
                    connected = false;
                    
                    // Try to reconnect
                    reconnect();
                }
            }
            
            // Sleep a short time to prevent CPU spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    void sendLoop() {
        while (running) {
            std::string msg;
            bool has_message = false;
            
            // Get next message to send
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
            
            // Send the message if we have one
            if (has_message && connected) {
                ssize_t bytes_sent = send(sockfd, msg.c_str(), msg.size(), 0);
                
                if (bytes_sent < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("send");
                        connected = false;
                        
                        // Requeue the message
                        {
                            std::lock_guard<std::mutex> lock(outgoing_mutex);
                            outgoing_queue.push(msg);
                        }
                        
                        // Try to reconnect
                        reconnect();
                    } else {
                        // Would block, try again later
                        std::lock_guard<std::mutex> lock(outgoing_mutex);
                        outgoing_queue.push(msg);
                    }
                }
            }
        }
    }
    
    void sendAcknowledgments() {
        while (running && connected) {
            // Sleep for the acknowledgment interval
            std::this_thread::sleep_for(std::chrono::milliseconds(ACK_INTERVAL_MS));
            
            if (!connected) {
                continue;
            }
            
            // Create acknowledgment
            Acknowledgment ack;
            ack.client_id = client_id;
            
            {
                std::lock_guard<std::mutex> lock(seq_mutex);
                ack.last_sequence_number = last_received_seq;
            }
            
            // Send acknowledgment
            std::string serialized = ack.serializeForNetwork();
            send(sockfd, serialized.c_str(), serialized.size(), 0);
        }
    }
    
    void processReceivedMessage(const std::string& data) {
        try {
            // Parse message
            Message msg = Message::deserialize(data);
            
            // Check sequence for order preservation
            {
                std::lock_guard<std::mutex> lock(seq_mutex);
                if (msg.sequence_number <= last_received_seq) {
                    // Already processed this message, ignore
                    return;
                }
                
                // Update last received sequence
                last_received_seq = msg.sequence_number;
            }
            
            // Update position cache
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                position_cache[msg.position.symbol] = msg.position.net_position;
            }
            
            // Log the position update
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
        
        // Close current socket
        close(sockfd);
        
        // Wait before attempting reconnection
        std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY_MS));
        
        // Try to connect again
        connectToServer();
        
        // If reconnection was successful, resume operation
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