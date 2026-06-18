FROM docker.m.daocloud.io/library/node:20-bookworm-slim AS build

WORKDIR /app

COPY web_gateway/package*.json ./web_gateway/

WORKDIR /app/web_gateway
RUN npm ci

COPY web_gateway/ ./
COPY protocol/ /app/protocol/

RUN npm run build \
    && npm prune --omit=dev

FROM docker.m.daocloud.io/library/node:20-bookworm-slim AS runtime

ENV NODE_ENV=production
ENV WEB_GATEWAY_HOST=0.0.0.0
ENV WEB_GATEWAY_PORT=3000
ENV IM_HOST=server
ENV IM_PORT=8080

WORKDIR /app/web_gateway

COPY --from=build /app/web_gateway/package*.json ./
COPY --from=build /app/web_gateway/node_modules ./node_modules
COPY --from=build /app/web_gateway/dist ./dist
COPY --from=build /app/web_gateway/public ./public
COPY --from=build /app/protocol /app/protocol

USER node
EXPOSE 3000

CMD ["npm", "run", "start"]
