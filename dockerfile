FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY core.hpp modules.hpp http.hpp json.hpp middleware.cpp worker.cpp ./
RUN mkdir /out \
 && g++ -std=c++17 -O2 -pthread -o /out/middleware middleware.cpp \
 && g++ -std=c++17 -O2 -pthread -o /out/worker worker.cpp

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 curl && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/middleware /out/worker /usr/local/bin/
CMD ["middleware"]
