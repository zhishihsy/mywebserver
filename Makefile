CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic -O2 -pthread
TARGET := webserver
SOURCES := main.cpp webserver.cpp http_connection.cpp \
	mysql_connection_pool.cpp user_repository.cpp
MYSQL_CFLAGS ?= $(shell pkg-config --cflags mysqlclient 2>/dev/null || pkg-config --cflags mariadb 2>/dev/null)
MYSQL_LIBS ?= $(shell pkg-config --libs mysqlclient 2>/dev/null || pkg-config --libs mariadb 2>/dev/null)

.PHONY: build mysql run stress test phase3 phase4 clean

build: $(TARGET)

$(TARGET): $(SOURCES) webserver.h http_connection.h thread_pool.h \
	mysql_connection_pool.h user_repository.h
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

mysql:
	@if [ -z "$(MYSQL_LIBS)" ]; then \
		echo "未通过 pkg-config 找到 MySQL/MariaDB 开发包"; \
		exit 1; \
	fi
	$(CXX) $(CXXFLAGS) -DENABLE_MYSQL $(MYSQL_CFLAGS) \
		$(SOURCES) -o $(TARGET) $(MYSQL_LIBS) -lcrypto

run: build
	./$(TARGET)

stress:
	python3 tests/stress_test.py

test: build
	python3 tests/http_phase2_test.py

phase3: build
	python3 tests/concurrency_phase3_test.py

phase4: mysql
	python3 tests/mysql_phase4_test.py

clean:
	rm -f $(TARGET)
