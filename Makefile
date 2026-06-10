CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic -O2 -pthread
TARGET := webserver
SOURCES := main.cpp webserver.cpp http_connection.cpp

.PHONY: build run stress test clean

build: $(TARGET)

$(TARGET): $(SOURCES) webserver.h http_connection.h thread_pool.h
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

run: build
	./$(TARGET)

stress:
	python3 tests/stress_test.py

test: build
	python3 tests/http_phase2_test.py

clean:
	rm -f $(TARGET)
