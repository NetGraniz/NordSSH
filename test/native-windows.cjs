// Actual packaged WebView2 + Rust IPC, on a fresh CI-only synthetic portable vault.
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
const net = require("node:net");
const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const { chromium } = require("playwright");
if (process.env.GITHUB_ACTIONS !== "true" || process.platform !== "win32")
  throw new Error("Native UI tests require isolated Windows CI");
const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
(async () => {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "NordSSH-native-"));
  const executable = path.join(root, "NordSSH.exe");
  await fs.copyFile("src-tauri/target/release/nordssh.exe", executable);
  await fs.copyFile(
    "assets/portable/nordssh.portable",
    path.join(root, "nordssh.portable"),
  );
  let child, browser;
  async function launch() {
    const listener = net.createServer();
    await new Promise((resolve) => listener.listen(0, "127.0.0.1", resolve));
    const port = listener.address().port;
    await new Promise((resolve) => listener.close(resolve));
    child = spawn(executable, [], {
      windowsHide: true,
      env: {
        ...process.env,
        WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port} --remote-debugging-address=127.0.0.1`,
        WEBVIEW2_USER_DATA_FOLDER: path.join(root, "webview-fixture"),
      },
    });
    for (let attempt = 0; attempt < 60; attempt++) {
      if (child.exitCode !== null)
        throw new Error("Native application exited early");
      try {
        const response = await fetch(`http://127.0.0.1:${port}/json/version`);
        if (response.ok) break;
      } catch {}
      await wait(500);
    }
    browser = await chromium.connectOverCDP(`http://127.0.0.1:${port}`);
    const context = browser.contexts()[0];
    let page = context.pages()[0];
    if (!page) page = await context.waitForEvent("page", { timeout: 15000 });
    page.setDefaultTimeout(15000);
    return page;
  }
  async function stop() {
    await browser?.close();
    browser = undefined;
    if (child && child.exitCode === null) {
      child.kill();
      await new Promise((resolve) => child.once("exit", resolve));
    }
    await wait(500);
  }
  try {
    let page = await launch();
    await page.locator("#vault-dialog[open]").waitFor();
    await page.locator("#vault-password").fill("short");
    await page.locator("#vault-confirm").fill("short");
    await page.locator("#vault-submit").click();
    await page
      .getByText("Пароль слишком короткий: нужно минимум 7 символов.", {
        exact: true,
      })
      .waitFor();
    await page.locator("#vault-password").fill("test-77");
    await page.locator("#vault-confirm").fill("test-78");
    await page.locator("#vault-submit").click();
    await page
      .getByText(
        "Повтор пароля не совпадает. Введи тот же пароль во втором поле.",
        { exact: true },
      )
      .waitFor();
    await page.locator("#vault-confirm").fill("test-77");
    await page.locator("#vault-submit").click();
    await page.locator("#vault-dialog").waitFor({ state: "hidden" });
    await page.locator("#add-host").click();
    await page.locator("[name=name]").first().fill("Native fixture");
    await page.locator("[name=hostname]").fill("127.0.0.1");
    await page.locator("[name=username]").fill("fixture");
    await page
      .locator("[name=password]")
      .first()
      .fill("synthetic-native-secret");
    await page.locator("#host-form button[type=submit]").click();
    await page.locator(".host-card").waitFor();
    await page.locator("[data-page=settings]").click();
    await page.locator("#theme").selectOption("midnight");
    await page.locator("#auto-updates").uncheck();
    await page.waitForFunction(() =>
      document.body.classList.contains("theme-midnight"),
    );
    await wait(500);
    const vaultPath = path.join(root, "NordSSH-data/vault.nordvault");
    assert.equal(
      (await fs.readFile(vaultPath, "utf8")).includes(
        "synthetic-native-secret",
      ),
      false,
    );
    await stop();
    page = await launch();
    await page.getByText("Native fixture", { exact: true }).waitFor();
    assert.equal(await page.locator("#vault-dialog").isVisible(), false);
    assert.equal(
      await page
        .locator("body")
        .evaluate((body) => body.classList.contains("theme-midnight")),
      true,
    );
    await page.locator("[data-page=settings]").click();
    await page.locator("#lock-vault").click();
    await page.locator("#vault-dialog[open]").waitFor();
    await page.locator("#vault-password").fill("wrong-password");
    await page.locator("#vault-submit").click();
    await page
      .locator("#vault-error")
      .filter({ hasText: "Wrong password" })
      .waitFor();
    await page.locator("#vault-password").fill("test-77");
    await page.locator("#vault-submit").click();
    await page.locator("#vault-dialog").waitFor({ state: "hidden" });
    console.log(
      "PASS: native Windows WebView2, real Rust IPC, 7-character vault, separate validation errors, encrypted host save, remembered reopen, settings persistence and wrong-password recovery. No IPC mocks.",
    );
  } finally {
    await stop();
  }
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
