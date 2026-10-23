import { invoke, Channel } from "@tauri-apps/api/core";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import type {
  Host,
  Snapshot,
  TerminalEvent,
  Settings,
  RemoteEntry,
} from "./types";
import { needsPasteConfirmation, remoteJoin, boundedFontSize } from "./policy";
import "./styles.css";
import { vaultUI } from "./vault-ui";

const app = document.querySelector<HTMLDivElement>("#app")!;
app.innerHTML = `
<header><div class="brand">Nord<span>SSH</span></div><span class="version" id="version">development</span><div class="spacer"></div><button class="flat" id="updates">Проверить обновления</button><button class="primary" id="add-host">＋ Подключение</button></header>
<aside><div class="sidebar-label">Рабочее пространство</div><button class="flat active" data-page="hosts">▦ &nbsp; Подключения</button><button class="flat" data-page="terminal">›_ &nbsp; Терминалы <span id="session-count"></span></button><button class="flat" data-page="snippets">⌘ &nbsp; Быстрые команды</button><div class="sidebar-label">Приложение</div><button class="flat" data-page="settings">⚙ &nbsp; Настройки</button><div class="sidebar-note"><span class="secure-dot"></span> Локальное хранилище<br>Переносимый зашифрованный файл.<br>Без облачной учётной записи.</div></aside>
<main><section class="page" id="hosts-page"><div class="page-top"><div><h2>Подключения</h2><span class="muted">Твои серверы. В одном месте.</span></div></div><div class="tools"><input id="search" placeholder="Поиск по имени, адресу или хранилищу" aria-label="Поиск серверов"><button id="favorites">☆ Избранное</button></div><div class="cards" id="hosts"></div><div class="empty" id="empty-hosts"><div class="empty-mark">&gt;_</div><h2>Первое подключение</h2><p>Добавь адрес сервера и способ входа. NordSSH запомнит подключение, а пароль сохранит только с твоего разрешения.</p><button class="primary" id="first-host">Добавить сервер</button></div></section>
<section id="terminal-page" hidden style="flex:1;min-height:0;flex-direction:column"><div class="tabs" id="tabs"></div><div class="terminal-toolbar"><span class="secure-dot"></span><span id="terminal-status">Нет подключений</span><div class="spacer"></div><button id="toggle-sftp">SFTP</button><button id="paste">Вставить</button></div><div class="terminal-area" id="terminal-area"><div class="empty" id="empty-terminal"><div class="empty-mark">&gt;_</div><h2>Здесь будет терминал</h2><p>Подключись к серверу из списка слева. Демонстрационных подключений здесь нет.</p></div></div><div class="sftp" id="sftp" hidden><div class="sftp-toolbar"><button id="sftp-up" title="Родительская папка">↑</button><input id="sftp-path" value="." aria-label="Удалённый каталог"><button id="sftp-go">Открыть</button><button id="sftp-upload">Загрузить файл</button></div><div id="sftp-status" class="muted"></div><table><thead><tr><th>Имя</th><th>Размер</th><th></th></tr></thead><tbody id="sftp-rows"></tbody></table></div></section>
<section class="page" id="snippets-page" hidden><div class="page-top"><div><h2>Быстрые команды</h2><span class="muted">Вставка в активный терминал — без автоматического выполнения.</span></div><div class="spacer"></div><button class="primary" id="add-snippet">＋ Команда</button></div><div id="snippets"></div></section>
<section class="page" id="settings-page" hidden><div class="settings"><h2>Настройки</h2><div class="setting-row"><div>Цветовая тема<br><small>Интерфейс и терминал</small></div><select id="theme"><option value="nord">Nord · зелёный</option><option value="midnight">Midnight · фиолетовый</option></select></div><div class="setting-row"><div>Размер шрифта терминала</div><input id="font-size" type="number" min="11" max="24" value="14"></div><div class="setting-row"><label for="auto-updates">Проверять обновления при запуске</label><input id="auto-updates" type="checkbox"></div><div class="notice">Данные хранятся только локально. Пароль нужен для переноса и восстановления; на своём устройстве разблокировку можно запомнить. Сохрани пароль отдельно от программы.</div><h3>Обновления</h3><p class="muted">Кнопка сверху проверяет GitHub Releases. Новая версия скачивается и устанавливается после твоего подтверждения и проверки подписи. Сначала закрой SSH-подключения и передачи файлов. Перед обновлением программа сохраняет зашифрованную резервную копию хранилища.</p></div></section></main>
<footer><span id="footer-status">Загрузка хранилища…</span><div class="spacer"></div><span>NordSSH · Windows / Linux</span></footer>
<dialog id="host-dialog"><form id="host-form"><h2 id="host-title">Новое подключение</h2><div class="form-grid"><label class="field full">Название<input name="name" required maxlength="256" placeholder="Мой сервер"></label><label class="field">Адрес<input name="hostname" required maxlength="256" placeholder="192.168.1.10"></label><label class="field">Порт<input name="port" type="number" value="22" min="1" max="65535" required></label><label class="field">Пользователь<input name="username" required maxlength="256" placeholder="administrator"></label><label class="field">Хранилище<input name="vault" value="Личное" required maxlength="256"></label><label class="field full">Способ входа<select name="auth"><option value="password">Пароль</option><option value="key">Приватный ключ</option><option value="certificate">Ключ + сертификат OpenSSH</option></select></label><label class="field full" id="password-field">Пароль<input name="password" type="password" autocomplete="off" placeholder="Оставь пустым, чтобы сохранить прежний"></label><div class="field full" id="key-field" hidden><label>Приватный ключ</label><div class="key-picker"><input name="keyPath" readonly aria-label="Приватный ключ"><button type="button" id="choose-key">Выбрать</button></div><label>Парольная фраза ключа</label><input name="passphrase" type="password" autocomplete="off"></div><div class="field full" id="cert-field" hidden><label>Сертификат OpenSSH</label><div class="key-picker"><input name="certificatePath" readonly aria-label="Сертификат"><button type="button" id="choose-cert">Выбрать</button></div></div><label class="field inline"><input name="saveSecret" type="checkbox" checked> Сохранить доступ в Windows</label><label class="field inline"><input name="favorite" type="checkbox"> Избранное</label></div><p class="error" id="host-error"></p><div class="form-actions"><button type="button" id="cancel-host">Отмена</button><button class="primary" type="submit">Сохранить</button></div></form></dialog>
<dialog id="credential-dialog"><form id="credential-form"><h2>Данные для подключения</h2><p class="muted">Сохранённый доступ не подошёл или отсутствует. Эти данные используются только для одной попытки подключения.</p><label class="field">Пароль / парольная фраза<input id="one-time-secret" type="password" autocomplete="off"></label><div class="form-actions"><button type="button" id="cancel-credential">Отмена</button><button type="submit" class="primary">Подключиться</button></div></form></dialog>
<dialog id="snippet-dialog"><form id="snippet-form"><h2>Быстрая команда</h2><label class="field">Название<input name="name" required maxlength="256"></label><br><label class="field">Текст<textarea name="command" required rows="5" maxlength="16384" spellcheck="false"></textarea></label><p class="muted">Команда вставится в терминал. Для выполнения нажми Enter.</p><div class="form-actions"><button type="button" id="cancel-snippet">Отмена</button><button type="submit" class="primary">Сохранить</button></div></form></dialog>`;

function el<T extends HTMLElement = HTMLElement>(id: string): T {
  return document.getElementById(id) as T;
}
const saveSecretLabel = app.querySelector<HTMLInputElement>(
  'input[name="saveSecret"]',
)!.parentElement!;
saveSecretLabel.lastChild!.textContent = " Сохранить доступ в хранилище";
function action(label: string, run: () => void, cls = ""): HTMLButtonElement {
  const b = document.createElement("button");
  b.textContent = label;
  b.className = cls;
  b.onclick = run;
  return b;
}
function message(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
function toast(text: string) {
  document.querySelector(".toast")?.remove();
  const t = document.createElement("div");
  t.className = "toast";
  t.textContent = text;
  t.setAttribute("role", "status");
  document.body.append(t);
  setTimeout(() => t.remove(), 9000);
}
const invokeSafe = <T>(name: string, args?: Record<string, unknown>) =>
  invoke<T>(name, args);
let snapshot: Snapshot;
let activePage = "hosts";
let favoritesOnly = false;
let editingHost: Host | undefined;
let activeSession: string | undefined;
interface Tab {
  id: string;
  host: Host;
  terminal: Terminal;
  fit: FitAddon;
  pane: HTMLElement;
  connected: boolean;
  connecting: boolean;
}
const tabs = new Map<string, Tab>();
const encoder = new TextEncoder();

function navigate(page: string) {
  activePage = page;
  for (const p of ["hosts", "terminal", "snippets", "settings"]) {
    el(`${p}-page`).hidden = p !== page;
  }
  el("terminal-page").style.display = page === "terminal" ? "flex" : "none";
  document
    .querySelectorAll<HTMLButtonElement>("[data-page]")
    .forEach((b) => b.classList.toggle("active", b.dataset.page === page));
  if (page === "terminal") setTimeout(fitActive, 40);
}
function applySettings() {
  document.body.classList.toggle(
    "theme-midnight",
    snapshot.settings.theme === "midnight",
  );
  el<HTMLSelectElement>("theme").value = snapshot.settings.theme;
  el<HTMLInputElement>("font-size").value = String(snapshot.settings.fontSize);
  el<HTMLInputElement>("auto-updates").checked = snapshot.settings.checkUpdates;
  for (const tab of tabs.values()) {
    tab.terminal.options.fontSize = snapshot.settings.fontSize;
    tab.terminal.options.theme = terminalTheme();
  }
  fitActive();
}
function terminalTheme() {
  return {
    background: snapshot.settings.theme === "midnight" ? "#12111e" : "#10161d",
    foreground: "#d8e3ea",
    cursor: snapshot.settings.theme === "midnight" ? "#b3a3f5" : "#82d7bc",
    selectionBackground: "#42566a",
  };
}
async function refresh() {
  snapshot = await invokeSafe<Snapshot>("snapshot");
  el("version").textContent = snapshot.version;
  renderHosts();
  renderSnippets();
  applySettings();
  renderTrustedKeys();
}
function renderTrustedKeys() {
  let section = document.getElementById("trusted-server-keys");
  if (!section) {
    section = document.createElement("section");
    section.id = "trusted-server-keys";
    document.querySelector(".settings")!.append(section);
  }
  section.replaceChildren();
  const heading = document.createElement("h3");
  heading.textContent = "Доверенные ключи серверов";
  section.append(heading);
  const entries = Object.entries(snapshot.knownHosts ?? {});
  if (!entries.length) {
    const text = document.createElement("p");
    text.className = "muted";
    text.textContent =
      "Ключ появится здесь после подтверждения отпечатка при подключении.";
    section.append(text);
  }
  for (const [endpoint, fingerprint] of entries) {
    const row = document.createElement("div");
    row.className = "setting-row";
    const text = document.createElement("div");
    text.textContent = endpoint;
    const key = document.createElement("small");
    key.textContent = fingerprint;
    text.append(document.createElement("br"), key);
    row.append(
      text,
      action(
        "Удалить доверие",
        () =>
          void invokeSafe<boolean>("forget_host_key", { endpoint, fingerprint })
            .then(() => refresh())
            .catch((error) => toast(message(error))),
      ),
    );
    section.append(row);
  }
}
function renderHosts() {
  const query = el<HTMLInputElement>("search").value.trim().toLocaleLowerCase();
  const cards = el("hosts");
  cards.replaceChildren();
  const hosts = snapshot.hosts.filter(
    (h) =>
      (!favoritesOnly || h.favorite) &&
      `${h.name} ${h.hostname} ${h.vault}`.toLocaleLowerCase().includes(query),
  );
  el("empty-hosts").hidden = snapshot.hosts.length > 0;
  for (const h of hosts) {
    const card = document.createElement("article");
    card.className = "host-card";
    const icon = document.createElement("div");
    icon.className = "host-icon";
    icon.textContent = "›_";
    const heading = document.createElement("h3");
    heading.textContent = `${h.favorite ? "★ " : ""}${h.name}`;
    const address = document.createElement("div");
    address.className = "host-address";
    address.textContent = `${h.username}@${h.hostname}:${h.port}`;
    const bottom = document.createElement("div");
    bottom.className = "host-bottom";
    const badge = document.createElement("span");
    badge.className = "badge";
    badge.textContent = h.vault;
    const spacer = document.createElement("div");
    spacer.className = "spacer";
    bottom.append(
      badge,
      spacer,
      action("Открыть", () => void connectHost(h), "primary"),
      action("⋯", () => editHost(h)),
    );
    card.append(icon, heading, address, bottom);
    cards.append(card);
  }
  if (!hosts.length && snapshot.hosts.length) {
    const p = document.createElement("p");
    p.className = "muted";
    p.textContent = "Подходящих подключений нет.";
    cards.append(p);
  }
  el("footer-status").textContent =
    `Локально · ${snapshot.hosts.length} подключений`;
}
const hostForm = el<HTMLFormElement>("host-form");
function field(name: string): HTMLInputElement {
  return hostForm.elements.namedItem(name) as HTMLInputElement;
}
function authFields() {
  el("password-field").hidden = field("auth").value !== "password";
  el("key-field").hidden = field("auth").value === "password";
  el("cert-field").hidden = field("auth").value !== "certificate";
}
function editHost(host?: Host) {
  editingHost = host;
  hostForm.reset();
  el("host-error").textContent = "";
  el("host-title").textContent = host
    ? "Изменить подключение"
    : "Новое подключение";
  if (host)
    for (const [name, value] of Object.entries(host)) {
      const f = hostForm.elements.namedItem(name) as HTMLInputElement | null;
      if (f) {
        if (f.type === "checkbox") f.checked = Boolean(value);
        else f.value = String(value);
      }
    }
  authFields();
  const oldDelete = el("host-dialog").querySelector("[data-delete]");
  oldDelete?.remove();
  if (host) {
    const b = action("Удалить подключение", () => {
      if (confirm(`Удалить «${host.name}» и сохранённый доступ?`))
        void invokeSafe("delete_host", { id: host.id })
          .then(() => {
            el<HTMLDialogElement>("host-dialog").close();
            return refresh();
          })
          .catch((e) => toast(message(e)));
    });
    b.dataset.delete = "true";
    hostForm.append(b);
  }
  el<HTMLDialogElement>("host-dialog").showModal();
}
hostForm.onsubmit = async (event) => {
  event.preventDefault();
  const host: Host = {
    id: editingHost?.id ?? crypto.randomUUID(),
    name: field("name").value.trim(),
    hostname: field("hostname").value.trim(),
    port: Number(field("port").value),
    username: field("username").value.trim(),
    vault: field("vault").value.trim(),
    auth: field("auth").value as Host["auth"],
    keyPath: field("keyPath").value,
    certificatePath: field("certificatePath").value,
    favorite: field("favorite").checked,
  };
  const password = field("password").value,
    passphrase = field("passphrase").value;
  const save = field("saveSecret").checked;
  try {
    await invokeSafe("save_host", {
      host,
      secret:
        save && (password || passphrase) ? { password, passphrase } : null,
      forgetSecret: !save,
    });
    field("password").value = "";
    field("passphrase").value = "";
    el<HTMLDialogElement>("host-dialog").close();
    await refresh();
  } catch (e) {
    el("host-error").textContent = message(e);
  }
};
el<HTMLDialogElement>("host-dialog").addEventListener("close", () => {
  field("password").value = "";
  field("passphrase").value = "";
});
field("auth").onchange = authFields;
for (const [button, name] of [
  ["choose-key", "keyPath"],
  ["choose-cert", "certificatePath"],
])
  el(button).onclick = () => {
    void invokeSafe<string | null>("pick_key")
      .then((path) => {
        if (path) field(name).value = path;
      })
      .catch((e) => toast(message(e)));
  };
for (const id of ["add-host", "first-host"]) el(id).onclick = () => editHost();
el("cancel-host").onclick = () => el<HTMLDialogElement>("host-dialog").close();
el("search").oninput = renderHosts;
el("favorites").onclick = () => {
  favoritesOnly = !favoritesOnly;
  el("favorites").classList.toggle("primary", favoritesOnly);
  renderHosts();
};
document
  .querySelectorAll<HTMLButtonElement>("[data-page]")
  .forEach((b) => (b.onclick = () => navigate(b.dataset.page!)));

function fitActive() {
  const t = activeSession ? tabs.get(activeSession) : undefined;
  if (t && activePage === "terminal") {
    t.fit.fit();
    t.terminal.focus();
  }
}
function selectTab(id: string) {
  activeSession = id;
  const tab = tabs.get(id)!;
  for (const t of tabs.values()) t.pane.classList.toggle("active", t.id === id);
  renderTabs();
  el("terminal-status").textContent =
    `${tab.host.username}@${tab.host.hostname} · ${tab.connecting ? "подключение…" : tab.connected ? "SSH" : "закрыто"}`;
  el("sftp").hidden = true;
  navigate("terminal");
}
function renderTabs() {
  const root = el("tabs");
  root.replaceChildren();
  for (const t of tabs.values()) {
    const b = action(
      `${t.connecting ? "◌" : t.connected ? "●" : "○"} ${t.host.name}`,
      () => selectTab(t.id),
      `tab${activeSession === t.id ? " active" : ""}`,
    );
    const x = document.createElement("span");
    x.className = "close-tab";
    x.textContent = "×";
    x.setAttribute("role", "button");
    x.setAttribute("aria-label", "Закрыть вкладку");
    x.onclick = (e) => {
      e.stopPropagation();
      void closeTab(t);
    };
    b.append(x);
    root.append(b);
  }
  el("session-count").textContent = tabs.size ? `(${tabs.size})` : "";
  el("empty-terminal").hidden = tabs.size > 0;
}
async function closeTab(tab: Tab) {
  if (tab.connecting) {
    toast(
      "Дождись результата подключения. Тайм-аут — до двух минут с подтверждением ключа.",
    );
    return;
  }
  if (
    tab.connected &&
    !confirm(
      `Закрыть SSH-сессию «${tab.host.name}»? Активная передача также может прерваться.`,
    )
  )
    return;
  if (tab.connected)
    await invokeSafe("disconnect", { id: tab.id }).catch((e) =>
      toast(message(e)),
    );
  tab.terminal.dispose();
  tab.pane.remove();
  tabs.delete(tab.id);
  if (activeSession === tab.id) {
    activeSession = tabs.keys().next().value;
    if (activeSession) selectTab(activeSession);
  }
  renderTabs();
}
async function connectHost(
  host: Host,
  secret?: { password: string; passphrase: string },
) {
  const id = crypto.randomUUID();
  const pane = document.createElement("div");
  pane.className = "terminal-pane";
  el("terminal-area").append(pane);
  const terminal = new Terminal({
    cursorBlink: true,
    fontFamily: "Cascadia Mono, Consolas, monospace",
    fontSize: snapshot.settings.fontSize,
    scrollback: 4000,
    theme: terminalTheme(),
    allowProposedApi: false,
  });
  const fit = new FitAddon();
  terminal.loadAddon(fit);
  terminal.open(pane);
  const tab: Tab = {
    id,
    host,
    terminal,
    fit,
    pane,
    connected: false,
    connecting: true,
  };
  tabs.set(id, tab);
  selectTab(id);
  terminal.writeln("\x1b[90mПодключение к серверу…\x1b[0m");
  terminal.onData((data) => {
    if (tab.connected)
      void invokeSafe("terminal_input", {
        id,
        data: Array.from(encoder.encode(data)),
      }).catch((e) => toast(message(e)));
  });
  terminal.onResize(({ cols, rows }) => {
    if (tab.connected)
      void invokeSafe("terminal_resize", { id, cols, rows }).catch(() => {});
  });
  terminal.attachCustomKeyEventHandler((event) => {
    if (
      event.type === "keydown" &&
      ((event.ctrlKey && event.shiftKey && event.code === "KeyV") ||
        (event.shiftKey && event.code === "Insert") ||
        (event.ctrlKey && event.code === "KeyV"))
    ) {
      void pasteClipboard(tab);
      return false;
    }
    if (
      event.type === "keydown" &&
      event.ctrlKey &&
      event.shiftKey &&
      event.code === "KeyC"
    ) {
      void navigator.clipboard
        .writeText(terminal.getSelection())
        .catch((e) => toast(message(e)));
      return false;
    }
    return true;
  });
  pane.addEventListener("contextmenu", (event) => {
    event.preventDefault();
    void pasteClipboard(tab);
  });
  pane.addEventListener(
    "paste",
    (event) => {
      event.preventDefault();
      event.stopImmediatePropagation();
      void pasteText(tab, event.clipboardData?.getData("text") ?? "");
    },
    true,
  );
  const output = new Channel<TerminalEvent>();
  output.onmessage = (event) => {
    if (event.kind === "data") {
      terminal.write(new Uint8Array(event.data), () => {
        void invokeSafe("terminal_ack", { id, bytes: event.data.length }).catch(
          () => {},
        );
      });
    } else {
      tab.connected = false;
      tab.connecting = false;
      terminal.writeln(`\r\n\x1b[90m${event.message}\x1b[0m`);
      renderTabs();
      if (activeSession === id)
        el("terminal-status").textContent = "Соединение закрыто";
    }
  };
  try {
    await invokeSafe("connect", {
      hostId: host.id,
      id,
      secret: secret ?? null,
      output,
    });
    tab.connected = true;
    tab.connecting = false;
    renderTabs();
    if (activeSession === id) {
      el("terminal-status").textContent =
        `${host.username}@${host.hostname} · SSH`;
      fitActive();
    }
  } catch (e) {
    tab.connecting = false;
    terminal.writeln(`\r\n\x1b[31m${message(e)}\x1b[0m`);
    renderTabs();
    if (/authentication|credentials|password/iu.test(message(e)))
      promptCredential(host);
  }
}
let retryHost: Host | undefined;
function promptCredential(host: Host) {
  retryHost = host;
  el<HTMLInputElement>("one-time-secret").value = "";
  el<HTMLDialogElement>("credential-dialog").showModal();
}
el<HTMLFormElement>("credential-form").onsubmit = (event) => {
  event.preventDefault();
  const value = el<HTMLInputElement>("one-time-secret").value;
  el<HTMLInputElement>("one-time-secret").value = "";
  el<HTMLDialogElement>("credential-dialog").close();
  if (retryHost)
    void connectHost(retryHost, {
      password: retryHost.auth === "password" ? value : "",
      passphrase: retryHost.auth === "password" ? "" : value,
    });
};
el("cancel-credential").onclick = () => {
  el<HTMLInputElement>("one-time-secret").value = "";
  el<HTMLDialogElement>("credential-dialog").close();
};
el<HTMLDialogElement>("credential-dialog").addEventListener("close", () => {
  el<HTMLInputElement>("one-time-secret").value = "";
});
async function pasteText(tab: Tab, text: string) {
  if (!tab.connected || !text) return;
  if (new TextEncoder().encode(text).length > 60000) {
    toast("Вставка ограничена 60 КБ. Для файлов используй SFTP.");
    return;
  }
  if (
    needsPasteConfirmation(text) &&
    !confirm(
      "Текст содержит переносы строк или управляющие символы. Он может выполнить команды. Вставить?",
    )
  )
    return;
  tab.terminal.paste(text);
  tab.terminal.focus();
}
async function pasteClipboard(tab: Tab) {
  try {
    await pasteText(tab, await navigator.clipboard.readText());
  } catch {
    toast(
      "Не удалось прочитать буфер. Используй Ctrl+Shift+V или проверь разрешение буфера обмена.",
    );
  }
}
el("paste").onclick = () => {
  const t = activeSession && tabs.get(activeSession);
  if (t) void pasteClipboard(t);
};
new ResizeObserver(fitActive).observe(el("terminal-area"));

let listing = 0;
async function listFiles() {
  const id = activeSession;
  if (!id || !tabs.get(id)?.connected) {
    toast("Сначала открой SSH-подключение.");
    return;
  }
  const generation = ++listing;
  const path = el<HTMLInputElement>("sftp-path").value || ".";
  el("sftp-status").textContent = "Чтение каталога…";
  try {
    const rows = await invokeSafe<RemoteEntry[]>("sftp_list", { id, path });
    if (generation !== listing || activeSession !== id) return;
    el("sftp-rows").replaceChildren();
    for (const row of rows) {
      const tr = document.createElement("tr");
      const name = document.createElement("td");
      const size = document.createElement("td");
      const buttons = document.createElement("td");
      name.textContent = `${row.directory ? "▸" : "·"} ${row.name}`;
      size.textContent = row.directory
        ? "Каталог"
        : `${row.size.toLocaleString()} Б`;
      buttons.append(
        action(row.directory ? "Открыть" : "Скачать", () => {
          try {
            const target = remoteJoin(path, row.name);
            if (row.directory) {
              el<HTMLInputElement>("sftp-path").value = target;
              void listFiles();
            } else {
              el("sftp-status").textContent = "Скачивание…";
              void invokeSafe<boolean>("sftp_download", { id, path: target })
                .then((done) => {
                  el("sftp-status").textContent = done
                    ? "Файл сохранён"
                    : "Отменено";
                })
                .catch((e) => {
                  el("sftp-status").textContent = message(e);
                });
            }
          } catch (e) {
            toast(message(e));
          }
        }),
      );
      tr.append(name, size, buttons);
      el("sftp-rows").append(tr);
    }
    el("sftp-status").textContent =
      `${rows.length} элементов · загрузка не перезаписывает существующие удалённые файлы`;
  } catch (e) {
    if (generation === listing) el("sftp-status").textContent = message(e);
  }
}
el("toggle-sftp").onclick = () => {
  el("sftp").hidden = !el("sftp").hidden;
  if (!el("sftp").hidden) void listFiles();
  setTimeout(fitActive, 50);
};
el("sftp-go").onclick = () => void listFiles();
el("sftp-up").onclick = () => {
  const path = el<HTMLInputElement>("sftp-path");
  path.value = `${path.value.replace(/\/$/u, "")}/..`;
  void listFiles();
};
el("sftp-path").onkeydown = (e) => {
  if (e.key === "Enter") void listFiles();
};
el("sftp-upload").onclick = () => {
  const id = activeSession;
  if (!id) return;
  el("sftp-status").textContent = "Загрузка…";
  void invokeSafe<boolean>("sftp_upload", {
    id,
    path: el<HTMLInputElement>("sftp-path").value || ".",
  })
    .then((done) => {
      if (done) return listFiles();
      el("sftp-status").textContent = "Отменено";
    })
    .catch((e) => {
      el("sftp-status").textContent = message(e);
    });
};

function renderSnippets() {
  const root = el("snippets");
  root.replaceChildren();
  for (const s of snapshot.snippets) {
    const row = document.createElement("div");
    row.className = "snippet";
    const text = document.createElement("div");
    const name = document.createElement("h3");
    name.textContent = s.name;
    const code = document.createElement("code");
    code.textContent = s.command;
    text.append(name, code);
    const spacer = document.createElement("div");
    spacer.className = "spacer";
    row.append(
      text,
      spacer,
      action("Вставить", () => {
        const t = activeSession ? tabs.get(activeSession) : undefined;
        if (!t?.connected)
          return toast("Сначала выбери подключённый терминал.");
        navigate("terminal");
        void pasteText(t, s.command);
      }),
      action("Удалить", () => {
        if (confirm(`Удалить команду «${s.name}»?`))
          void invokeSafe("delete_snippet", { id: s.id })
            .then(refresh)
            .catch((e) => toast(message(e)));
      }),
    );
    root.append(row);
  }
  if (!snapshot.snippets.length) {
    const p = document.createElement("p");
    p.className = "muted";
    p.textContent =
      "Сохрани часто используемые команды. Они никогда не запускаются при подключении сами.";
    root.append(p);
  }
}
el("add-snippet").onclick = () => {
  el<HTMLFormElement>("snippet-form").reset();
  el<HTMLDialogElement>("snippet-dialog").showModal();
};
el("cancel-snippet").onclick = () =>
  el<HTMLDialogElement>("snippet-dialog").close();
el<HTMLFormElement>("snippet-form").onsubmit = async (event) => {
  event.preventDefault();
  const form = el<HTMLFormElement>("snippet-form");
  const name = (form.elements.namedItem("name") as HTMLInputElement).value;
  const command = (form.elements.namedItem("command") as HTMLTextAreaElement)
    .value;
  try {
    await invokeSafe("save_snippet", {
      snippet: { id: crypto.randomUUID(), name, command },
    });
    el<HTMLDialogElement>("snippet-dialog").close();
    await refresh();
  } catch (e) {
    toast(message(e));
  }
};
async function saveAppearance() {
  const settings: Settings = {
    theme: el<HTMLSelectElement>("theme").value as Settings["theme"],
    fontSize: boundedFontSize(Number(el<HTMLInputElement>("font-size").value)),
    checkUpdates: el<HTMLInputElement>("auto-updates").checked,
  };
  try {
    await invokeSafe("save_settings", { settings });
    snapshot.settings = settings;
    applySettings();
  } catch (e) {
    toast(message(e));
  }
}
for (const id of ["theme", "font-size", "auto-updates"])
  el(id).onchange = () => void saveAppearance();
let checking = false;
let availableUpdate: string | undefined;
let installing = false;
async function installUpdate() {
  if (installing || checking) return;
  installing = true;
  const dialog = document.createElement("dialog");
  dialog.innerHTML = `<h2>Обновление NordSSH</h2><p id="update-progress-text">Ожидаю подтверждение установки…</p><progress id="update-progress" aria-label="Загрузка обновления"></progress><p class="muted">Хранилище останется на месте. После установки программа перезапустится.</p>`;
  dialog.id = "update-dialog";
  dialog.addEventListener("cancel", (event) => event.preventDefault());
  document.body.append(dialog);
  dialog.showModal();
  const output = new Channel<{
    phase: string;
    received: number;
    total: number | null;
  }>();
  output.onmessage = (event) => {
    const progress = el<HTMLProgressElement>("update-progress");
    if (event.total) {
      progress.max = event.total;
      progress.value = event.received;
    }
    el("update-progress-text").textContent =
      event.phase === "verify"
        ? "Проверяю подпись и версию…"
        : event.phase === "install"
          ? "Устанавливаю обновление…"
          : `Скачиваю обновление: ${(event.received / 1048576).toFixed(1)} МБ${event.total ? ` из ${(event.total / 1048576).toFixed(1)} МБ` : ""}`;
  };
  try {
    const result = await invokeSafe<string>("install_update", { output });
    if (result === "current") {
      availableUpdate = undefined;
      el("updates").textContent = "Обновлений нет";
    }
    if (result !== "restarting" && result !== "cancelled")
      toast("Обновление завершено.");
  } catch (error) {
    toast(message(error));
  } finally {
    dialog.close();
    dialog.remove();
    installing = false;
  }
}
async function checkUpdates(quiet = false) {
  if (checking || installing) return;
  checking = true;
  const b = el<HTMLButtonElement>("updates");
  b.disabled = true;
  b.textContent = "Проверка…";
  try {
    const result = await invokeSafe<{ status: string; version: string }>(
      "check_updates",
    );
    b.textContent =
      result.status === "available"
        ? `Обновить до ${result.version}`
        : "Обновлений нет";
    availableUpdate =
      result.status === "available" ? result.version : undefined;
    if (!quiet)
      toast(
        result.status === "no-release"
          ? "Обновлений пока нет."
          : result.status === "available"
            ? `Есть версия ${result.version}. Нажми «Обновить» для установки подписанного пакета.`
            : "Установлена актуальная версия.",
      );
  } catch (e) {
    b.textContent = "Повторить проверку";
    if (!quiet) toast(message(e));
  } finally {
    checking = false;
    b.disabled = false;
  }
}
el("updates").onclick = () =>
  void (availableUpdate ? installUpdate() : checkUpdates());
void vaultUI(
  async () => {
    await refresh();
    if (snapshot.settings.checkUpdates) void checkUpdates(true);
  },
  () => {
    for (const tab of tabs.values()) {
      tab.terminal.dispose();
      tab.pane.remove();
    }
    tabs.clear();
    activeSession = undefined;
    renderTabs();
    snapshot = {
      hosts: [],
      snippets: [],
      settings: { theme: "nord", fontSize: 14, checkUpdates: false },
      version: snapshot.version,
    };
    renderHosts();
    renderSnippets();
    hostForm.reset();
  },
  toast,
).catch((e) => {
  el("footer-status").textContent = "Не удалось открыть хранилище";
  toast(message(e));
  el<HTMLButtonElement>("add-host").disabled = true;
  el<HTMLButtonElement>("first-host").disabled = true;
});
