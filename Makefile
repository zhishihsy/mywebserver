CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic -O2 -pthread
SANITIZER_FLAGS := -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined
TARGET := webserver
SOURCES := main.cpp webserver.cpp http_connection.cpp logger.cpp \
	mysql_connection_pool.cpp user_repository.cpp
MYSQL_CFLAGS ?= $(shell pkg-config --cflags mysqlclient 2>/dev/null || pkg-config --cflags mariadb 2>/dev/null)
MYSQL_LIBS ?= $(shell pkg-config --libs mysqlclient 2>/dev/null || pkg-config --libs mariadb 2>/dev/null)

.PHONY: build mysql sanitizer run stress load-test test test-all \
	test-http test-concurrency test-stability test-mysql test-logging clean

build: $(TARGET)

$(TARGET): $(SOURCES) webserver.h http_connection.h thread_pool.h \
	logger.h blocking_queue.h \
	mysql_connection_pool.h user_repository.h
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

mysql:
	@if [ -z "$(MYSQL_LIBS)" ]; then \
		echo "未通过 pkg-config 找到 MySQL/MariaDB 开发包"; \
		exit 1; \
	fi
	$(CXX) $(CXXFLAGS) -DENABLE_MYSQL $(MYSQL_CFLAGS) \
		$(SOURCES) -o $(TARGET) $(MYSQL_LIBS) -lcrypto

sanitizer:
	$(CXX) $(CXXFLAGS) $(SANITIZER_FLAGS) $(SOURCES) -o $(TARGET)

run: build
	./$(TARGET)

stress:
	python3 tests/stress_test.py

load-test:
	python3 tests/load_test.py --connections 10000

test: test-http

test-all: test-http test-concurrency test-stability test-logging

test-http: build
	python3 tests/http_protocol_test.py

test-concurrency: build
	python3 tests/concurrency_test.py

test-stability: build
	python3 tests/stability_test.py

test-mysql: mysql
	python3 tests/mysql_integration_test.py

test-logging: build
	$(CXX) $(CXXFLAGS) tests/logger_unit_test.cpp logger.cpp \
		-o tests/logger_unit_test
	./tests/logger_unit_test
	rm -f tests/logger_unit_test
	python3 tests/logging_integration_test.py

clean:
	rm -f $(TARGET) tests/logger_unit_test tests/logger_unit_test.exe
