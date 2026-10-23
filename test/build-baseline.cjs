// CI-only older build for real installer/portable upgrade tests. Never published.
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
if (process.env.GITHUB_ACTIONS !== "true" || process.platform !== "win32")
  throw new Error("Baseline builder runs only in isolated Windows CI");
const files = [
  "src-tauri/Cargo.toml",
  "src-tauri/Cargo.lock",
  "src-tauri/tauri.conf.json",
];
const original = files.map((file) => fs.readFileSync(file, "utf8"));
try {
  fs.writeFileSync(
    files[0],
    original[0].replace(
      /^(version = ")[^"]+("\r?$)/m,
      (_, prefix, suffix) => prefix + "0.0.0" + suffix,
    ),
  );
  const config = JSON.parse(original[2]);
  config.version = "0.0.0";
  fs.writeFileSync(files[2], JSON.stringify(config, null, 2));
  const result = spawnSync(
    process.execPath,
    ["node_modules/@tauri-apps/cli/tauri.js", "build", "--bundles", "nsis"],
    { stdio: "inherit" },
  );
  if (result.status !== 0) throw new Error("Baseline build failed");
  fs.mkdirSync("artifacts/baseline", { recursive: true });
  fs.copyFileSync(
    "src-tauri/target/release/nordssh.exe",
    "artifacts/baseline/NordSSH.exe",
  );
  const installer = fs
    .readdirSync("src-tauri/target/release/bundle/nsis")
    .find((name) => name.includes("_0.0.0_") && name.endsWith("-setup.exe"));
  if (!installer) throw new Error("Baseline installer missing");
  fs.copyFileSync(
    path.join("src-tauri/target/release/bundle/nsis", installer),
    "artifacts/baseline/setup.exe",
  );
} finally {
  files.forEach((file, index) => fs.writeFileSync(file, original[index]));
}
