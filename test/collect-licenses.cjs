// Packaging helper: copies upstream license/notice texts, never application data.
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
(async () => {
  const destination = path.resolve(process.argv[2] || "artifacts/licenses");
  const registry = path.join(
    process.env.CARGO_HOME || path.join(os.homedir(), ".cargo"),
    "registry",
    "src",
  );
  const roots = (await fs.readdir(registry, { withFileTypes: true }))
    .filter((e) => e.isDirectory())
    .map((e) => path.join(registry, e.name));
  const lock = await fs.readFile("src-tauri/Cargo.lock", "utf8");
  let copied = 0;
  async function copyNotices(root, label) {
    const entries = await fs.readdir(root, { withFileTypes: true });
    for (const entry of entries) {
      if (
        !entry.isFile() ||
        !/^(LICENSE|COPYING|COPYRIGHT|NOTICE)([._-].*)?$/iu.test(entry.name)
      )
        continue;
      const target = path.join(destination, label);
      await fs.mkdir(target, { recursive: true });
      await fs.copyFile(
        path.join(root, entry.name),
        path.join(target, entry.name),
      );
      copied++;
    }
  }
  for (const block of lock.split("[[package]]").slice(1)) {
    if (!block.includes("registry+")) continue;
    const name = block.match(/^name = "([^"]+)"/m)[1],
      version = block.match(/^version = "([^"]+)"/m)[1];
    for (const root of roots) {
      const folder = path.join(root, name + "-" + version);
      try {
        await fs.access(folder);
        await copyNotices(folder, name + "-" + version);
        break;
      } catch (e) {
        if (e.code !== "ENOENT") throw e;
      }
    }
  }
  await copyNotices("vendor/glib-0.18.5", "glib-0.18.5-patched");
  for (const name of ["@xterm/xterm", "@xterm/addon-fit", "@tauri-apps/api"])
    await copyNotices(path.join("node_modules", name), name.replace("/", "-"));
  await fs.writeFile(
    path.join(destination, "README.txt"),
    "Third-party license and copyright notices for components in the Windows/Linux builds. Not every component is used on every platform. The GLib security backport retains the upstream MIT license; see source vendor/PATCHES.md.\n",
  );
  console.log("Collected " + copied + " upstream license/notice files.");
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
