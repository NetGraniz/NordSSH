import { invoke } from "@tauri-apps/api/core";
interface Status {
  initialized: boolean;
  unlocked: boolean;
  migration: boolean;
  path: string;
  portable: boolean;
}
export async function vaultUI(
  onUnlock: () => Promise<void>,
  onLock: () => void,
  notify: (text: string) => void,
) {
  const dialog = document.createElement("dialog");
  dialog.id = "vault-dialog";
  dialog.innerHTML = `<form id="vault-form"><h2 id="vault-title"></h2><p id="vault-description" class="muted"></p><label class="field">Пароль хранилища<input id="vault-password" type="password" autocomplete="off" required maxlength="4096"></label><br><label class="field" id="vault-confirm-field">Повтори пароль<input id="vault-confirm" type="password" autocomplete="off" maxlength="4096"></label><br><label class="field inline" id="vault-remember-field"><input id="vault-remember" type="checkbox" checked> Запомнить на этом устройстве</label><p class="error" id="vault-error" role="alert"></p><div class="form-actions"><button type="button" id="vault-cancel">Отмена</button><button class="primary" type="submit" id="vault-submit">Продолжить</button></div><button type="button" class="flat" id="vault-restore">Восстановить из файла</button></form>`;
  document.body.append(dialog);
  const get = <T extends HTMLElement>(id: string) =>
    document.getElementById(id) as T;
  const form = get<HTMLFormElement>("vault-form");
  let status = await invoke<Status>("vault_status");
  let operation: "open" | "export" | "import" = "open";
  let busy = false;
  function inert(locked: boolean) {
    for (const element of document.querySelectorAll<HTMLElement>(
      "header,aside,main",
    ))
      element.inert = locked;
  }
  function show(mode: typeof operation) {
    if (busy) return;
    operation = mode;
    form.reset();
    get("vault-error").textContent = "";
    const create = mode === "open" && !status.initialized;
    get("vault-title").textContent =
      mode === "export"
        ? "Экспорт хранилища"
        : mode === "import"
          ? "Импорт хранилища"
          : create
            ? "Создай переносимое хранилище"
            : "Открой хранилище";
    get("vault-description").textContent =
      mode === "export"
        ? "Файл включает пароли, ключи и настройки. Задай пароль для его защиты; он понадобится при импорте."
        : mode === "import"
          ? "Введи пароль экспортированного файла. Затем выбери файл. Текущие данные будут заменены с резервной копией."
          : create
            ? (status.migration
                ? "Данные старого Windows-хранилища перенесутся в новый формат. Оригинал останется нетронутым. "
                : "") +
              "Пароль нужен для переноса и восстановления. Запиши его: без него восстановить доступ на другом устройстве нельзя. На этом компьютере его можно запомнить."
            : "При переносе на новый компьютер введи пароль один раз. Запоминание не переносится вместе с файлом.";
    get("vault-confirm-field").hidden = !(create || mode === "export");
    get<HTMLInputElement>("vault-confirm").required =
      create || mode === "export";
    get("vault-remember-field").hidden = mode === "export";
    get("vault-cancel").hidden = mode === "open";
    get("vault-restore").hidden = mode !== "open";
    if (!dialog.open) dialog.showModal();
    get<HTMLInputElement>("vault-password").focus();
  }
  async function activate() {
    status = await invoke<Status>("vault_status");
    await onUnlock();
    inert(false);
    dialog.close();
    form.reset();
  }
  form.onsubmit = async (event) => {
    event.preventDefault();
    if (busy) return;
    const password = get<HTMLInputElement>("vault-password").value;
    const confirmation = get<HTMLInputElement>("vault-confirm").value;
    const create = operation === "open" && !status.initialized;
    if (
      (create || operation === "export") &&
      (password !== confirmation || [...password].length < 12)
    ) {
      get("vault-error").textContent =
        "Пароли должны совпадать и содержать минимум 12 символов.";
      return;
    }
    busy = true;
    get<HTMLButtonElement>("vault-submit").disabled = true;
    get("vault-error").textContent = "";
    try {
      const remember = get<HTMLInputElement>("vault-remember").checked;
      if (operation === "export") {
        const done = await invoke<boolean>("vault_export", { password });
        if (done) notify("Зашифрованный файл сохранён. Храни пароль отдельно.");
        dialog.close();
        form.reset();
      } else {
        const warning =
          operation === "open"
            ? await invoke<string | null>("vault_unlock", {
                password,
                remember,
                create,
              })
            : await invoke<string | null>("vault_import", {
                password,
                remember,
              });
        await activate();
        if (warning) notify(warning);
      }
    } catch (e) {
      get("vault-error").textContent = String(e);
    } finally {
      get<HTMLInputElement>("vault-password").value = "";
      get<HTMLInputElement>("vault-confirm").value = "";
      busy = false;
      get<HTMLButtonElement>("vault-submit").disabled = false;
    }
  };
  dialog.addEventListener("cancel", (e) => {
    if (operation === "open" || busy) e.preventDefault();
  });
  dialog.addEventListener("close", () => {
    form.reset();
    if (!status.unlocked) queueMicrotask(() => show("open"));
  });
  get("vault-cancel").onclick = () => {
    if (!busy) dialog.close();
  };
  get("vault-restore").onclick = () => show("import");
  const settings = document.querySelector(".settings")!;
  const section = document.createElement("section");
  section.innerHTML = `<h3>Локальное хранилище</h3><p class="muted" id="vault-location"></p><div class="actions"><button id="export-vault">Экспорт</button><button id="import-vault">Импорт</button><button id="lock-vault">Заблокировать</button><button id="forget-device">Забыть на устройстве</button></div><p class="muted">Хранилище не отправляется в облако. Резервные копии делай вручную. Portable-режим хранит данные рядом с программой, но не переносит запомненный ключ разблокировки.</p>`;
  settings.prepend(section);
  get("vault-location").textContent =
    `${status.portable ? "Portable" : "Обычный режим"} · ${status.path}`;
  get("export-vault").onclick = () => show("export");
  get("import-vault").onclick = () => show("import");
  get("forget-device").onclick = () => {
    if (
      confirm(
        "Удалить запомненный ключ на этом устройстве? При следующем запуске понадобится пароль хранилища.",
      )
    )
      void invoke("vault_forget_device")
        .then(() => notify("Запомненный ключ удалён."))
        .catch((e) => notify(String(e)));
  };
  get("lock-vault").onclick = () => {
    void invoke("vault_lock")
      .then(() => {
        onLock();
        status.unlocked = false;
        inert(true);
        show("open");
      })
      .catch((e) => notify(String(e)));
  };
  if (status.unlocked) {
    await activate();
  } else {
    inert(true);
    show("open");
  }
}
