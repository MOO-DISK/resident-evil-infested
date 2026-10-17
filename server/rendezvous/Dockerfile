FROM python:3.12-alpine
WORKDIR /app
COPY rendezvous.py .
USER nobody
EXPOSE 27961/udp
CMD ["python3", "-u", "rendezvous.py", "--port", "27961"]
