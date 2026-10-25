FROM python:3.12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN pip install --no-cache-dir numpy pybind11 && make build && ./build/pao_tests

FROM python:3.12-slim
WORKDIR /app
RUN pip install --no-cache-dir numpy
COPY --from=build /app /app
ENV PYTHONPATH=/app/python PAO_ENGINE=/app/build/pao_engine
EXPOSE 8080
CMD ["python", "-m", "pao", "serve", "--host", "0.0.0.0", "--port", "8080", "--db", "/app/pao.db"]
