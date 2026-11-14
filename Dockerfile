# --- Stage 1: Build stage ---
FROM ubuntu:22.04 AS builder

# Prevent prompt blockers during apt-get install
ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies needed for C++ compilation
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory inside the container
WORKDIR /app

# Copy CMake files and source directories
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/

# Compile the binary in Release mode
RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make

# --- Stage 2: Runtime stage ---
FROM ubuntu:22.04

WORKDIR /app

# Copy the compiled binary from the builder stage
COPY --from=builder /app/build/CacheCore ./

# Copy the example configuration file for reference
COPY store.example.conf ./store.example.conf

# Expose CacheCore's port (6948)
EXPOSE 6948

# Run CacheCore — requires configuration file path via CLI argument
ENTRYPOINT ["./CacheCore"]
