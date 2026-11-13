# Skowork control plane: one Node.js process (API, push WebSocket, LLM gateway, admin console), SQLite on a volume.
#   docker build -t skowork-control-plane control-plane
#   docker run -p 8080:8080 -v skowork-data:/data --env-file .env -e SKOWORK_ENV=production skowork-control-plane
FROM node:24-alpine AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json ./
COPY src ./src
COPY admin ./admin
RUN npx tsc --noEmit -p tsconfig.json && npm run admin:build

FROM node:24-alpine
WORKDIR /app
ENV NODE_ENV=production SKOWORK_ENV=production SKOWORK_DB=/data/skowork.db PORT=8080
COPY package.json package-lock.json ./
RUN npm ci --omit=dev && npm cache clean --force
COPY src ./src
COPY --from=build /app/dist ./dist
ARG VERSION=0.0.0
LABEL org.opencontainers.image.title="Skowork control plane" org.opencontainers.image.version=$VERSION
RUN mkdir -p /data && chown node:node /data
VOLUME /data
USER node
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s CMD wget -qO- http://127.0.0.1:8080/healthz || exit 1
CMD ["node", "src/main.ts"]
