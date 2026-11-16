FROM node:20-alpine
RUN apk add --no-cache ghostscript
WORKDIR /app
COPY server.mjs .
ENV DATA_DIR=/app/data
VOLUME /app/data
EXPOSE 8080
CMD ["node", "server.mjs"]
