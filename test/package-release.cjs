// Build-output packaging only. No credentials or application data are copied.
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const version = require("../package.json").version;
const destination = path.resolve("artifacts/release");
fs.mkdirSync(destination, { recursive: true });
function run(exe, args) {
  const result = spawnSync(exe, args, { stdio: "inherit" });
  if (result.status !== 0) throw new Error("Packaging command failed: " + exe);
}
function sign(file) {
  run(process.execPath, [
    "node_modules/@tauri-apps/cli/tauri.js",
    "signer",
    "sign",
    "--app-version",
    version,
    file,
  ]);
}
function bundle(folder, extension) {
  const matches = fs
    .readdirSync(folder)
    .filter(
      (name) => name.includes("_" + version + "_") && name.endsWith(extension),
    );
  if (matches.length !== 1) throw new Error("Ambiguous bundle: " + folder);
  return path.join(folder, matches[0]);
}
const portable = path.resolve("artifacts/portable-" + process.platform);
fs.mkdirSync(portable, { recursive: true });
for (const name of ["nordssh.portable", "README.txt"])
  fs.copyFileSync(
    path.join("assets/portable", name),
    path.join(portable, name),
  );
fs.cpSync("artifacts/licenses", path.join(portable, "licenses"), {
  recursive: true,
});
if (process.platform === "win32") {
  const payload = path.join(destination, `NordSSH-${version}-windows-x64.exe`);
  const setup = path.join(
    destination,
    `NordSSH-${version}-windows-x64-setup.exe`,
  );
  fs.copyFileSync("src-tauri/target/release/nordssh.exe", payload);
  fs.copyFileSync(payload, path.join(portable, "NordSSH.exe"));
  fs.copyFileSync(
    bundle("src-tauri/target/release/bundle/nsis", "-setup.exe"),
    setup,
  );
  sign(payload);
  sign(setup);
  run("tar", [
    "-a",
    "-cf",
    path.join(destination, `NordSSH-${version}-windows-x64-portable.zip`),
    "-C",
    portable,
    "NordSSH.exe",
    "nordssh.portable",
    "README.txt",
    "licenses",
  ]);
} else if (process.platform === "linux") {
  for (const [folder, extension, name] of [
    ["deb", ".deb", `NordSSH-${version}-linux-amd64.deb`],
    ["appimage", ".AppImage", `NordSSH-${version}-linux-x86_64.AppImage`],
  ]) {
    const file = path.join(destination, name);
    fs.copyFileSync(
      bundle("src-tauri/target/release/bundle/" + folder, extension),
      file,
    );
    sign(file);
    if (extension === ".AppImage") {
      fs.chmodSync(file, 0o755);
      fs.copyFileSync(file, path.join(portable, "NordSSH.AppImage"));
      fs.chmodSync(path.join(portable, "NordSSH.AppImage"), 0o755);
    }
  }
  run("tar", [
    "-czf",
    path.join(destination, `NordSSH-${version}-linux-x64-portable.tar.gz`),
    "-C",
    portable,
    "NordSSH.AppImage",
    "nordssh.portable",
    "README.txt",
    "licenses",
  ]);
} else throw new Error("Unsupported release platform");
