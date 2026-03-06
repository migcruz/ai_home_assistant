# ── Stage 1: Build frontend ──────────────────────────────────────────────────
FROM node:22-slim AS build
WORKDIR /build
COPY src/package.json src/package-lock.json* ./
RUN npm install
COPY src/ .
RUN npm run build

# ── Stage 2: Serve with nginx ────────────────────────────────────────────────
FROM nginx:alpine
# Arrange files to match the URL paths nginx will serve
COPY --from=build /build/dist/index.html /usr/share/nginx/html/voice/index.html
COPY --from=build /build/dist/assets     /usr/share/nginx/html/voice/static/assets
COPY nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
