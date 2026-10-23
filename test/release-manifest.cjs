const fs = require("node:fs");
const path = require("node:path");
const { createHash } = require("node:crypto");
const version = require("../package.json").version;
const root = path.resolve(process.argv[2] || "artifacts/release");
const platforms = {};
for (const [target, name] of [
  ["windows-x86_64-nsis", `NordSSH-${version}-windows-x64-setup.exe`],
  ["windows-x86_64-portable", `NordSSH-${version}-windows-x64.exe`],
  ["linux-x86_64-deb", `NordSSH-${version}-linux-amd64.deb`],
  ["linux-x86_64-appimage", `NordSSH-${version}-linux-x86_64.AppImage`],
]) {
  if (!fs.statSync(path.join(root, name)).isFile())
    throw new Error("Missing release asset: " + name);
  platforms[target] = {
    url: `https://github.com/NetGraniz/NordSSH/releases/download/v${version}/${name}`,
    signature: fs.readFileSync(path.join(root, name + ".sig"), "utf8").trim(),
  };
}
fs.writeFileSync(
  path.join(root, "latest.json"),
  JSON.stringify(
    {
      version,
      notes: "NordSSH " + version,
      pub_date: new Date().toISOString(),
      platforms,
    },
    null,
    2,
  ) + "\n",
);
const sums =
  fs
    .readdirSync(root)
    .filter(
      (name) =>
        name !== "SHA256SUMS.txt" &&
        fs.statSync(path.join(root, name)).isFile(),
    )
    .sort()
    .map(
      (name) =>
        `${createHash("sha256")
          .update(fs.readFileSync(path.join(root, name)))
          .digest("hex")}  ${name}`,
    )
    .join("\n") + "\n";
fs.writeFileSync(path.join(root, "SHA256SUMS.txt"), sums);
console.log(
  "Generated signed-update manifest and release checksums for " + version,
);
