// Browser-only UI test. IPC is mocked; this is not an SSH integration test.
const { chromium } = require("playwright");
const { spawn } = require("node:child_process");
const { mkdir, readFile } = require("node:fs/promises");
const assert = require("node:assert/strict");
const path = require("node:path");
(async () => {
  const vite = spawn(
    process.execPath,
    [
      path.resolve("node_modules/vite/bin/vite.js"),
      "--host",
      "127.0.0.1",
      "--port",
      "1420",
    ],
    { stdio: "ignore", windowsHide: true },
  );
  let browser;
  try {
    for (let i = 0; i < 100; i++) {
      try {
        const r = await fetch("http://127.0.0.1:1420");
        if (r.ok) break;
      } catch {}
      await new Promise((r) => setTimeout(r, 100));
    }
    console.log("UI fixture: Vite ready; launching isolated browser");
    browser = await chromium.launch({
      executablePath: "C:/Program Files/Google/Chrome/Application/chrome.exe",
      headless: true,
      timeout: 20000,
      args: ["--proxy-server=direct://", "--proxy-bypass-list=*"],
    });
    const page = await browser.newPage({
      viewport: { width: 1240, height: 820 },
    });
    const errors = [];
    page.on("pageerror", (error) => errors.push(error.message));
    await page.route("http://127.0.0.1:1420/", async (route) => {
      let html = await readFile("index.html", "utf8");
      html = html.replace(
        '<script type="module" src="/src/main.ts"></script>',
        `<script type="module">
      import {mockIPC} from '/node_modules/@tauri-apps/api/mocks.js';
      const state={hosts:[],settings:{theme:'nord',fontSize:14,checkUpdates:false},snippets:[],version:'UI test'};
      mockIPC((command,args)=>{
        if(command==='vault_status')return {initialized:true,unlocked:true,migration:false,path:'fixture.nordvault',portable:true};
        if(command==='snapshot')return structuredClone(state);
        if(command==='save_host'){state.hosts.push(args.host);return;}
        if(command==='save_settings'){state.settings=args.settings;return;}
        if(command==='save_snippet'){state.snippets.push(args.snippet);return;}
        if(command==='check_updates')return {status:'no-release',version:''};
        throw new Error('Unexpected mock command '+command);
      });
      await import('/src/main.ts');</script>`,
      );
      await route.fulfill({ contentType: "text/html", body: html });
    });
    page.setDefaultTimeout(15000);
    console.log("UI fixture: browser ready");
    await page.goto("http://127.0.0.1:1420", {
      waitUntil: "domcontentloaded",
      timeout: 20000,
    });
    await page.getByText("Локально · 0 подключений", { exact: true }).waitFor();
    await mkdir("test-results", { recursive: true });
    await page.screenshot({ path: "test-results/hosts-empty.png" });
    await page.locator("#add-host").click();
    await page.locator("[name=name]").first().fill("Fixture server");
    await page.locator("[name=hostname]").fill("127.0.0.1");
    await page.locator("[name=username]").fill("fixture");
    await page.locator("[name=auth]").selectOption("key");
    assert.equal(await page.locator("#password-field").isVisible(), false);
    assert.equal(await page.locator("#key-field").isVisible(), true);
    await page.locator("[name=auth]").selectOption("password");
    await page.screenshot({ path: "test-results/new-host.png" });
    await page.locator("#host-form button[type=submit]").click();
    await page.locator(".host-card").waitFor();
    await page.screenshot({ path: "test-results/hosts.png" });
    assert.equal(
      await page.locator(".host-card h3").textContent(),
      "Fixture server",
    );
    await page.locator("#search").fill("not-found");
    assert.equal(await page.locator(".host-card").count(), 0);
    await page.locator("#search").fill("");
    await page.locator("[data-page=settings]").click();
    await page.locator("#theme").selectOption("midnight");
    await page.waitForFunction(() =>
      document.body.classList.contains("theme-midnight"),
    );
    await page.screenshot({ path: "test-results/settings.png" });
    await page.locator("[data-page=snippets]").click();
    await page.locator("#add-snippet").click();
    await page.locator("#snippet-form [name=name]").fill("System status");
    await page.locator("#snippet-form [name=command]").fill("uptime");
    await page.locator("#snippet-form button[type=submit]").click();
    await page.locator(".snippet").waitFor();
    await page.locator("#updates").click();
    await page.getByText("Обновлений нет", { exact: true }).waitFor();
    assert.deepEqual(errors, []);
    console.log(
      "PASS: empty state, host form/auth fields, host rendering/search, theme, snippets, update check; no browser errors. IPC mocked.",
    );
  } finally {
    await browser?.close();
    vite.kill();
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
