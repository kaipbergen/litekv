FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    g++ cmake make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN rm -rf build && mkdir build && cd build && cmake .. && make

EXPOSE 6380

ENTRYPOINT ["./build/litekv"]