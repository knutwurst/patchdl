const API = {
  status: "/api/status",
  titles: "/api/titles",
  config: "/api/config",
  downloads: "/api/downloads",
  action: (titleId, action) => `/api/titles/${encodeURIComponent(titleId)}/${action}`,
};

const fallback = {
  status: {
    firmware: "11.60",
    firmware_build: "0x11600000",
    dns_guard: "Aktiv",
    resolver: "Internal allowlist",
    free_space_mb: 64218,
    download_dir: "/mnt/usb0/patches",
  },
  config: {
    default_policy: "deny",
    download_dir: "/mnt/usb0/patches",
    install_after_download: false,
    delete_pkg_after_install: false,
    cdn_allowlist: [
      "sgst.prod.dl.playstation.net",
      "gst.prod.dl.playstation.net",
      "gs2.ww.prod.dl.playstation.net",
    ],
  },
  titles: [
    {
      title_id: "PPSA01628_00",
      name: "Call of Duty Black Ops Cold War",
      content_id: "UP0002-PPSA01628_00-CODCWTHEGAME0001",
      installed_version: "01.032.000",
      compatible_version: "01.041.000",
      latest_version: "01.041.000",
      latest_required_fw: "11.60",
      enabled: true,
      mode: "latest_compatible",
      queued: false,
      status: "available",
    },
    {
      title_id: "PPSA01284_00",
      name: "Demon's Souls",
      content_id: "UP9000-PPSA01284_00-DEMONSSOULS00001",
      installed_version: "01.002.000",
      compatible_version: "01.004.000",
      latest_version: "01.004.000",
      latest_required_fw: "10.01",
      enabled: true,
      mode: "pin",
      max_content_ver: "01.004.000",
      queued: true,
      status: "queued",
    },
    {
      title_id: "PPSA08329_00",
      name: "Example Future Game",
      content_id: "EP0000-PPSA08329_00-FUTUREPATCH00001",
      installed_version: "01.000.000",
      compatible_version: null,
      latest_version: "01.012.000",
      latest_required_fw: "12.50",
      enabled: false,
      mode: "disabled",
      queued: false,
      status: "blocked",
    },
  ],
  downloads: [
    {
      title_id: "PPSA01284_00",
      name: "Demon's Souls",
      version: "01.004.000",
      progress: 64,
      detail: "4.8 GB von 7.5 GB",
    },
  ],
  logs: [
    "[12:41:02] nanoDNS guard detected: Sony domains blocked",
    "[12:41:04] Internal resolver ready for 3 CDN hosts",
    "[12:41:07] PPSA01284_00 selected: pin 01.004.000",
    "[12:41:09] Download started: PPSA01284_00 01.004.000",
  ],
};

let state = {
  status: fallback.status,
  config: fallback.config,
  titles: fallback.titles,
  downloads: fallback.downloads,
  logs: fallback.logs,
  filter: "all",
  query: "",
  usingFallback: false,
};

const els = {};

document.addEventListener("DOMContentLoaded", () => {
  bindElements();
  bindEvents();
  loadInitialData();
});

function bindElements() {
  Object.assign(els, {
    firmwareValue: document.getElementById("firmwareValue"),
    firmwareBuild: document.getElementById("firmwareBuild"),
    dnsValue: document.getElementById("dnsValue"),
    resolverValue: document.getElementById("resolverValue"),
    spaceValue: document.getElementById("spaceValue"),
    downloadDirValue: document.getElementById("downloadDirValue"),
    gameGrid: document.getElementById("gameGrid"),
    queueList: document.getElementById("queueList"),
    logOutput: document.getElementById("logOutput"),
    allowlistHosts: document.getElementById("allowlistHosts"),
    searchInput: document.getElementById("searchInput"),
    defaultPolicy: document.getElementById("defaultPolicy"),
    downloadDir: document.getElementById("downloadDir"),
    installAfterDownload: document.getElementById("installAfterDownload"),
    deleteAfterInstall: document.getElementById("deleteAfterInstall"),
    refreshBtn: document.getElementById("refreshBtn"),
    saveBtn: document.getElementById("saveBtn"),
    pauseAllBtn: document.getElementById("pauseAllBtn"),
    toast: document.getElementById("toast"),
  });
}

function bindEvents() {
  els.searchInput.addEventListener("input", (event) => {
    state.query = event.target.value.trim().toLowerCase();
    renderGames();
  });

  document.querySelectorAll(".segmented button").forEach((button) => {
    button.addEventListener("click", () => {
      document.querySelectorAll(".segmented button").forEach((item) => item.classList.remove("is-selected"));
      button.classList.add("is-selected");
      state.filter = button.dataset.filter;
      renderGames();
    });
  });

  els.refreshBtn.addEventListener("click", loadInitialData);
  els.saveBtn.addEventListener("click", saveConfig);
  els.pauseAllBtn.addEventListener("click", () => showToast("Queue wurde pausiert."));
}

async function loadInitialData() {
  const [status, config, titles, downloads] = await Promise.all([
    getJson(API.status, fallback.status),
    getJson(API.config, fallback.config),
    getJson(API.titles, fallback.titles),
    getJson(API.downloads, fallback.downloads),
  ]);

  state = {
    ...state,
    status,
    config,
    titles,
    downloads,
  };

  render();
  showToast(state.usingFallback ? "Demo-Daten geladen. API noch nicht erreichbar." : "Daten aktualisiert.");
}

async function getJson(url, fallbackValue) {
  try {
    const response = await fetch(url, { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return await response.json();
  } catch (error) {
    state.usingFallback = true;
    return clone(fallbackValue);
  }
}

function render() {
  renderStatus();
  renderSettings();
  renderGames();
  renderQueue();
  renderLogs();
}

function renderStatus() {
  els.firmwareValue.textContent = state.status.firmware || "--";
  els.firmwareBuild.textContent = state.status.firmware_build || "Systemversion";
  els.dnsValue.textContent = state.status.dns_guard || "--";
  els.resolverValue.textContent = state.status.resolver || "--";
  els.spaceValue.textContent = formatMb(state.status.free_space_mb);
  els.downloadDirValue.textContent = state.status.download_dir || state.config.download_dir || "Download Ziel";
}

function renderSettings() {
  els.defaultPolicy.value = state.config.default_policy || "deny";
  els.downloadDir.value = state.config.download_dir || "";
  els.installAfterDownload.checked = Boolean(state.config.install_after_download);
  els.deleteAfterInstall.checked = Boolean(state.config.delete_pkg_after_install);
  els.allowlistHosts.replaceChildren(...(state.config.cdn_allowlist || []).map((host) => {
    const chip = document.createElement("span");
    chip.className = "host-chip";
    chip.textContent = host;
    return chip;
  }));
}

function renderGames() {
  const visible = state.titles.filter(matchesFilter).filter(matchesQuery);
  els.gameGrid.replaceChildren();

  if (!visible.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "Keine Titel fuer diesen Filter.";
    els.gameGrid.appendChild(empty);
    return;
  }

  visible.forEach((game) => els.gameGrid.appendChild(createGameCard(game)));
}

function matchesFilter(game) {
  if (state.filter === "enabled") return game.enabled;
  if (state.filter === "available") return game.status === "available";
  if (state.filter === "blocked") return game.status === "blocked";
  if (state.filter === "queued") return game.queued || game.status === "queued";
  return true;
}

function matchesQuery(game) {
  if (!state.query) return true;
  return [game.name, game.title_id, game.content_id]
    .filter(Boolean)
    .some((value) => value.toLowerCase().includes(state.query));
}

function createGameCard(game) {
  const card = document.createElement("article");
  card.className = "game-card";

  const title = document.createElement("div");
  title.className = "game-title";
  title.innerHTML = `
    <div class="cover">${initials(game.name)}</div>
    <div>
      <h3>${escapeHtml(game.name)}</h3>
      <div class="subline">
        <span>${escapeHtml(game.title_id)}</span>
        <span>${escapeHtml(game.content_id || "")}</span>
      </div>
      <div class="subline" style="margin-top:8px">
        ${statusPill(game)}
        <span class="pill">${game.enabled ? "Aktiv" : "Aus"}</span>
      </div>
    </div>
  `;

  const versions = document.createElement("div");
  versions.className = "version-grid";
  versions.innerHTML = `
    ${versionBox("Installiert", game.installed_version || "--")}
    ${versionBox("Kompatibel", game.compatible_version || "Keine")}
    ${versionBox("Neueste", `${game.latest_version || "--"} / FW ${game.latest_required_fw || "--"}`)}
  `;

  const controls = document.createElement("div");
  controls.className = "controls";

  const controlRow = document.createElement("div");
  controlRow.className = "control-row";

  const mode = document.createElement("select");
  mode.title = "Update-Modus";
  [
    ["disabled", "Disabled"],
    ["download_only", "Download only"],
    ["latest_compatible", "Latest compatible"],
    ["pin", "Pin version"],
    ["check_only", "Check only"],
  ].forEach(([value, label]) => {
    const option = document.createElement("option");
    option.value = value;
    option.textContent = label;
    option.selected = (game.mode || "disabled") === value;
    mode.appendChild(option);
  });
  mode.addEventListener("change", () => updateGame(game.title_id, { mode: mode.value, enabled: mode.value !== "disabled" }));

  const pin = document.createElement("input");
  pin.title = "Maximale Content-Version";
  pin.placeholder = "max ver";
  pin.value = game.max_content_ver || "";
  pin.addEventListener("change", () => updateGame(game.title_id, { max_content_ver: pin.value.trim() }));

  controlRow.append(mode, pin);

  const actions = document.createElement("div");
  actions.className = "actions-row";
  actions.innerHTML = `
    <button class="row-button" data-action="check">
      <svg><use href="#icon-refresh"></use></svg>
      Pruefen
    </button>
    <button class="row-button is-primary" data-action="download" ${!game.compatible_version ? "disabled" : ""}>
      <svg><use href="#icon-download"></use></svg>
      Laden
    </button>
  `;

  actions.querySelectorAll("button").forEach((button) => {
    button.addEventListener("click", () => runTitleAction(game.title_id, button.dataset.action));
  });

  controls.append(controlRow, actions);
  card.append(title, versions, controls);
  return card;
}

function versionBox(label, value) {
  return `
    <div class="version-box">
      <span>${escapeHtml(label)}</span>
      <strong>${escapeHtml(value)}</strong>
    </div>
  `;
}

function statusPill(game) {
  if (game.status === "blocked") return `<span class="pill blocked">FW blockiert</span>`;
  if (game.status === "queued") return `<span class="pill warn">In Queue</span>`;
  if (game.status === "available") return `<span class="pill ok">Update verfuegbar</span>`;
  return `<span class="pill">Aktuell</span>`;
}

function renderQueue() {
  els.queueList.replaceChildren();
  if (!state.downloads.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "Keine aktiven Downloads.";
    els.queueList.appendChild(empty);
    return;
  }

  state.downloads.forEach((item) => {
    const row = document.createElement("div");
    row.className = "queue-item";
    row.innerHTML = `
      <div>
        <strong>${escapeHtml(item.name)}</strong>
        <span>${escapeHtml(item.title_id)} - ${escapeHtml(item.version)} - ${escapeHtml(item.detail || "")}</span>
        <div class="progress"><i style="width:${Math.max(0, Math.min(100, item.progress || 0))}%"></i></div>
      </div>
      <span>${item.progress || 0}%</span>
    `;
    els.queueList.appendChild(row);
  });
}

function renderLogs() {
  els.logOutput.textContent = state.logs.join("\n");
}

async function saveConfig() {
  const config = {
    ...state.config,
    default_policy: els.defaultPolicy.value,
    download_dir: els.downloadDir.value.trim(),
    install_after_download: els.installAfterDownload.checked,
    delete_pkg_after_install: els.deleteAfterInstall.checked,
  };

  try {
    await postJson(API.config, config);
    state.config = config;
    showToast("Konfiguration gespeichert.");
  } catch (error) {
    state.config = config;
    showToast("Konfiguration lokal aktualisiert. API noch nicht erreichbar.");
  }

  renderSettings();
}

async function runTitleAction(titleId, action) {
  const game = state.titles.find((item) => item.title_id === titleId);
  if (!game) return;

  try {
    await postJson(API.action(titleId, action), {});
  } catch (error) {
    // Demo mode keeps the UI interactive before the ELF API exists.
  }

  if (action === "download" && game.compatible_version) {
    game.queued = true;
    game.status = "queued";
    if (!state.downloads.some((item) => item.title_id === titleId)) {
      state.downloads.push({
        title_id: game.title_id,
        name: game.name,
        version: game.compatible_version,
        progress: 0,
        detail: "Wartet auf Start",
      });
    }
    state.logs.push(`[${timeNow()}] Queued ${game.title_id} ${game.compatible_version}`);
    showToast(`${game.title_id} wurde zur Queue hinzugefuegt.`);
  } else {
    state.logs.push(`[${timeNow()}] Check requested for ${game.title_id}`);
    showToast(`${game.title_id} wird geprueft.`);
  }

  renderGames();
  renderQueue();
  renderLogs();
}

function updateGame(titleId, patch) {
  const game = state.titles.find((item) => item.title_id === titleId);
  if (!game) return;
  Object.assign(game, patch);
  if (patch.mode === "disabled") game.enabled = false;
  state.logs.push(`[${timeNow()}] Policy updated for ${titleId}`);
  renderGames();
  renderLogs();
}

async function postJson(url, body) {
  const response = await fetch(url, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json().catch(() => ({}));
}

function initials(name) {
  return (name || "PD")
    .split(/\s+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((part) => part[0])
    .join("")
    .toUpperCase();
}

function formatMb(value) {
  if (!Number.isFinite(Number(value))) return "--";
  if (value > 1024) return `${(value / 1024).toFixed(1)} GB`;
  return `${value} MB`;
}

function escapeHtml(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function timeNow() {
  return new Date().toLocaleTimeString("de-DE", { hour12: false });
}

let toastTimer;
function showToast(message) {
  els.toast.textContent = message;
  els.toast.classList.add("is-visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => els.toast.classList.remove("is-visible"), 2600);
}
