CXX     = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17

TARGET = agent

all: $(TARGET)

$(TARGET): agent.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
