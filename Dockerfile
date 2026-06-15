# Stage 1: Build
FROM gcc:13 AS builder

RUN apt-get update && apt-get install -y cmake git

WORKDIR /app
COPY . .

RUN if [ ! -d "extern/httplib/.git" ] && [ ! -f "extern/httplib/CMakeLists.txt" ]; then \
        rm -rf extern/httplib && \
        git clone --depth 1 https://github.com/yhirose/cpp-httplib.git extern/httplib; \
    fi

RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DNANODB_BUILD_PYTHON=OFF \
             -DNANODB_BUILD_SERVER=ON && \
    cmake --build . --target nano_server -j$(nproc)

# Stage 2: Runtime
# Use debian:sid (unstable) which has GCC 13 packages
FROM debian:sid-slim

RUN apt-get update && \
    apt-get install -y libstdc++6 libgomp1 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/nano_server .

RUN mkdir -p data

EXPOSE 8080

ENV NANODB_PORT=8080
ENV NANODB_DATA_DIR=/app/data

CMD ["./nano_server"]
