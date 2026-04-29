#
# Makefile for the ChromeTrace MTI plugin
#
# Produces ChromeTrace.so which can be loaded by any FVP that supports MTI plugins.
#
# Usage:
#   export PVLIB_HOME=/path/to/FastModelsPortfolio_11.27
#   make
#
# Load the plugin with the FVP:
#   FVP_... --plugin InstProfiler.so                              \
#           -C TRACE.InstProfiler.symbol-file=my_app.elf       \
#           -C TRACE.InstProfiler.output-file=trace.json       \
#           -C TRACE.InstProfiler.demangle=1                   \
#           -C TRACE.InstProfiler.tid=1                        \
#           -C TRACE.InstProfiler.pid=1
#

PLUGIN  = InstProfiler

# Include paths: FVP runtime headers
CPPFLAGS = -I $(PVLIB_HOME)/include/fmruntime \
           -I $(PVLIB_HOME)/include/fmruntime/eslapi

# C++14 required; -fPIC mandatory for shared libraries.
# No -Werror so that vendored headers with warnings don't block the build.
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -fPIC

# Source files (one module per concern)
SRCS = InstProfiler.cpp \
       Coverage.cpp    \
       Flamegraph.cpp  \
       Lcov.cpp        \
       Stats.cpp       \
       JsonWriter.cpp  \
       SymbolTable.cpp

OBJS = $(SRCS:.cpp=.o)

# Default target
all: $(PLUGIN).so

# Compile each .cpp; rebuild if any header changes
%.o: %.cpp $(wildcard *.h)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# Link shared library
$(PLUGIN).so: $(OBJS)
	$(CXX) -pthread -ldl -lrt -shared -o $@ $(OBJS)

clean:
	rm -f $(OBJS) $(PLUGIN).so

# Use the C++ linker when CC is called
CC = $(CXX)

# End of Makefile
