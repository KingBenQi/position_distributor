CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O3 -pthread
DEBUG_FLAGS = -g -DDEBUG

all: position_server position_client

position_server: position_server.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o position_server position_server.cpp

position_client: position_client.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o position_client position_client.cpp

debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: all

clean:
	rm -f position_server position_client *.o position_server.log

.PHONY: all debug clean