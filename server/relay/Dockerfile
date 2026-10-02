FROM python:3.12-alpine
WORKDIR /app
COPY relay.py .
RUN adduser -D relay
USER relay
EXPOSE 3074/udp
CMD ["python", "-u", "relay.py"]
