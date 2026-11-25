CXX ?= c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic

backtester: main.cpp mbo.hpp backtest.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp $(LDFLAGS) -o $@

.PHONY: test clean

test: backtester
	python3 tests/test_backtester.py ./backtester

clean:
	rm -f backtester
