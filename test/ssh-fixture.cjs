// Loopback-only protocol fixture. No production files, commands or credentials.
const { Server, utils } = require("ssh2");
const { createHash } = require("node:crypto");
const { STATUS_CODE: STATUS, OPEN_MODE } = utils.sftp;
const keys = utils.generateKeyPairSync("ed25519");
const userKeys = utils.generateKeyPairSync("ed25519", {
  passphrase: "fixture-passphrase",
  cipher: "aes256-ctr",
});
const userKey = utils.parseKey(userKeys.private, "fixture-passphrase");
if (process.argv[2])
  require("node:fs").writeFileSync(process.argv[2], userKeys.private);
const fingerprint =
  "SHA256:" +
  createHash("sha256")
    .update(utils.parseKey(keys.private).getPublicSSH())
    .digest("base64")
    .replace(/=+$/u, "");
const files = new Map([["/hello.txt", Buffer.from("NordSSH SFTP fixture\n")]]);
const attrs = (size = 0, directory = false) => ({
  mode: directory ? 0o040755 : 0o100644,
  uid: 1000,
  gid: 1000,
  size,
  atime: 0,
  mtime: 0,
});
const server = new Server({ hostKeys: [keys.private] }, (client) => {
  client.on("error", () => {});
  client.on("authentication", (ctx) => {
    if (ctx.username !== "fixture") return ctx.reject();
    if (ctx.method === "password" && ctx.password === "synthetic-only")
      return ctx.accept();
    if (
      ctx.method === "publickey" &&
      ctx.key.data.equals(userKey.getPublicSSH())
    ) {
      if (
        !ctx.signature ||
        userKey.verify(ctx.blob, ctx.signature, ctx.hashAlgo) === true
      )
        return ctx.accept();
    }
    ctx.reject();
  });
  client.on("ready", () =>
    client.on("session", (accept) => {
      const session = accept();
      session.on("pty", (accept) => accept());
      session.on("window-change", (accept) => accept?.());
      session.on("shell", (accept) => {
        const stream = accept();
        stream.write("NORDSSH_FIXTURE_READY\r\n");
        stream.on("data", (chunk) =>
          stream.write(chunk.toString().includes("ping") ? "pong\r\n" : chunk),
        );
      });
      session.on("sftp", (accept) => {
        const sftp = accept();
        const handles = new Map();
        let sequence = 0;
        const handle = (value) => {
          const id = Buffer.alloc(4);
          id.writeUInt32BE(++sequence);
          handles.set(sequence, value);
          return id;
        };
        const get = (buffer) =>
          buffer.length === 4 ? handles.get(buffer.readUInt32BE(0)) : undefined;
        sftp.on("REALPATH", (id, path) =>
          sftp.name(id, [
            {
              filename: path === "." ? "/" : path,
              longname: "",
              attrs: attrs(0, true),
            },
          ]),
        );
        sftp.on("OPENDIR", (id) =>
          sftp.handle(id, handle({ directory: true, sent: false })),
        );
        sftp.on("READDIR", (id, h) => {
          const entry = get(h);
          if (!entry || entry.sent) return sftp.status(id, STATUS.EOF);
          entry.sent = true;
          sftp.name(
            id,
            [...files].map(([path, data]) => ({
              filename: path.slice(1),
              longname: "",
              attrs: attrs(data.length),
            })),
          );
        });
        for (const event of ["STAT", "LSTAT"])
          sftp.on(event, (id, path) =>
            files.has(path)
              ? sftp.attrs(id, attrs(files.get(path).length))
              : path === "/"
                ? sftp.attrs(id, attrs(0, true))
                : sftp.status(id, STATUS.NO_SUCH_FILE),
          );
        sftp.on("OPEN", (id, path, flags) => {
          if (flags & OPEN_MODE.EXCL && files.has(path))
            return sftp.status(id, STATUS.FAILURE);
          if (!files.has(path)) {
            if (!(flags & OPEN_MODE.CREAT))
              return sftp.status(id, STATUS.NO_SUCH_FILE);
            files.set(path, Buffer.alloc(0));
          }
          sftp.handle(id, handle({ path }));
        });
        sftp.on("READ", (id, h, offset, length) => {
          const entry = get(h);
          if (!entry) return sftp.status(id, STATUS.FAILURE);
          const data = files.get(entry.path);
          if (offset >= data.length) return sftp.status(id, STATUS.EOF);
          sftp.data(id, data.subarray(offset, offset + length));
        });
        sftp.on("WRITE", (id, h, offset, chunk) => {
          const entry = get(h);
          if (!entry || offset + chunk.length > 1024 * 1024)
            return sftp.status(id, STATUS.FAILURE);
          const old = files.get(entry.path);
          const data = Buffer.alloc(
            Math.max(old.length, offset + chunk.length),
          );
          old.copy(data);
          chunk.copy(data, offset);
          files.set(entry.path, data);
          sftp.status(id, STATUS.OK);
        });
        sftp.on("CLOSE", (id, h) => {
          if (h.length === 4) handles.delete(h.readUInt32BE(0));
          sftp.status(id, STATUS.OK);
        });
        sftp.on("FSTAT", (id, h) => {
          const entry = get(h);
          entry
            ? sftp.attrs(id, attrs(files.get(entry.path)?.length ?? 0))
            : sftp.status(id, STATUS.FAILURE);
        });
      });
    }),
  );
});
server.listen(0, "127.0.0.1", () =>
  process.stdout.write(
    JSON.stringify({ port: server.address().port, fingerprint }) + "\n",
  ),
);
