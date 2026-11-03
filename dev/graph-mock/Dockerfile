# dev/graph-mock container image (task 2.7): the Graph surface on 4810 and the control API on 4811. Development and CI
# only; it holds fixture data and never delivers mail.
FROM node:24-alpine
WORKDIR /mock
COPY package.json package-lock.json ./
RUN npm ci --omit=dev
COPY src ./src
COPY fixtures ./fixtures
USER node
EXPOSE 4810 4811
ENTRYPOINT ["node", "src/main.ts", "--host", "0.0.0.0"]
