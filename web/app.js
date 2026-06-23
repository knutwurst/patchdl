const API = {
  status: "/api/status",
  titles: "/api/titles",
  config: "/api/config",
  downloads: "/api/downloads",
  action: (titleId, action) => `/api/titles/${encodeURIComponent(titleId)}/${action}`,
};

const SOURCE_TYPES = {
  official: {
    label: "Official",
    className: "ok",
    installAllowed: true,
    description: "Native console installation",
  },
  external: {
    label: "External",
    className: "external",
    installAllowed: true,
    description: "Real external installation",
  },
  shadowmount: {
    label: "Shadowmount",
    className: "shadow",
    installAllowed: false,
    description: "Download only",
  },
  unknown: {
    label: "Unknown",
    className: "blocked",
    installAllowed: false,
    description: "Blocked",
  },
};

const fallback = {
  status: {
    firmware: "11.60",
    firmware_build: "0x11600000",
    dns_guard: "Active",
    resolver: "Internal allowlist",
    free_space_mb: 64218,
    download_dir: "/mnt/usb0/patches",
  },
  config: {
    default_policy: "deny",
    download_dir: "/mnt/usb0/patches",
    install_after_download: false,
    delete_pkg_after_install: false,
    source_policy: {
      official: { allow_check: true, allow_download: true, allow_install: true },
      external: { allow_check: true, allow_download: true, allow_install: true },
      shadowmount: { allow_check: true, allow_download: true, allow_install: false },
      unknown: { allow_check: true, allow_download: false, allow_install: false },
    },
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
      source_type: "official",
      source_path: "/system_ex/app/PPSA01628_00",
      mount_from: "/dev/ssd0.system_ex",
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
      source_type: "external",
      source_path: "/system_data/priv/appmeta/external/PPSA01284_00",
      mount_from: "/mnt/ext0/user/app/PPSA01284_00",
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
      source_type: "unknown",
      source_path: "/system_ex/app/PPSA08329_00",
      mount_from: "",
      enabled: false,
      mode: "disabled",
      queued: false,
      status: "blocked",
    },
    {
      title_id: "PPSA90001_00",
      name: "Shadowmounted Test Title",
      content_id: "UP0000-PPSA90001_00-SHADOWMOUNT0001",
      installed_version: "01.000.000",
      compatible_version: "01.006.000",
      latest_version: "01.009.000",
      latest_required_fw: "12.00",
      source_type: "shadowmount",
      source_path: "/system_ex/app/PPSA90001_00",
      mount_from: "/mnt/usb0/itemzflow/Shadowmounted Test Title",
      enabled: true,
      mode: "download_only",
      queued: false,
      status: "available",
    },
  ],
  downloads: [
    {
      title_id: "PPSA01284_00",
      name: "Demon's Souls",
      version: "01.004.000",
      progress: 64,
      detail: "4.8 GB of 7.5 GB",
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
    sourcePolicyList: document.getElementById("sourcePolicyList"),
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
  els.pauseAllBtn.addEventListener("click", () => showToast("Queue paused."));
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
  showToast(state.usingFallback ? "Demo data loaded. API is not reachable yet." : "Data refreshed.");
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
  els.firmwareBuild.textContent = state.status.firmware_build || "System version";
  els.dnsValue.textContent = state.status.dns_guard || "--";
  els.resolverValue.textContent = state.status.resolver || "--";
  els.spaceValue.textContent = formatMb(state.status.free_space_mb);
  els.downloadDirValue.textContent = state.status.download_dir || state.config.download_dir || "Download target";
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
  renderSourcePolicies();
}

function renderGames() {
  const visible = state.titles.filter(matchesFilter).filter(matchesQuery);
  els.gameGrid.replaceChildren();

  if (!visible.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "No titles match this filter.";
    els.gameGrid.appendChild(empty);
    return;
  }

  visible.forEach((game) => els.gameGrid.appendChild(createGameCard(game)));
}

function matchesFilter(game) {
  if (state.filter === "enabled") return game.enabled;
  if (state.filter === "available") return game.status === "available";
  if (state.filter === "shadowmount") return sourceType(game) === "shadowmount";
  if (state.filter === "blocked") return game.status === "blocked";
  if (state.filter === "queued") return game.queued || game.status === "queued";
  return true;
}

function matchesQuery(game) {
  if (!state.query) return true;
  return [game.name, game.title_id, game.content_id]
    .concat([game.source_type, game.source_path, game.mount_from])
    .filter(Boolean)
    .some((value) => value.toLowerCase().includes(state.query));
}

function createGameCard(game) {
  const card = document.createElement("article");
  card.className = "game-card";

  const title = document.createElement("div");
  title.className = "game-title";
  const installBlocked = isInstallBlocked(game);
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
        ${sourcePill(game)}
        ${installBlocked ? `<span class="pill blocked">Install blocked</span>` : `<span class="pill ok">Install allowed</span>`}
        <span class="pill">${game.enabled ? "Enabled" : "Off"}</span>
      </div>
      <div class="source-path">${escapeHtml(sourceDetail(game))}</div>
    </div>
  `;

  const versions = document.createElement("div");
  versions.className = "version-grid";
  versions.innerHTML = `
    ${versionBox("Installed", game.installed_version || "--")}
    ${versionBox("Compatible", game.compatible_version || "None")}
    ${versionBox("Latest", `${game.latest_version || "--"} / FW ${game.latest_required_fw || "--"}`)}
  `;

  const controls = document.createElement("div");
  controls.className = "controls";

  const controlRow = document.createElement("div");
  controlRow.className = "control-row";

  const mode = document.createElement("select");
  mode.title = "Update mode";
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
  pin.title = "Maximum content version";
  pin.placeholder = "max ver";
  pin.value = game.max_content_ver || "";
  pin.addEventListener("change", () => updateGame(game.title_id, { max_content_ver: pin.value.trim() }));

  controlRow.append(mode, pin);

  const actions = document.createElement("div");
  actions.className = "actions-row";
  actions.innerHTML = `
    <button class="row-button" data-action="check">
      <svg><use href="#icon-refresh"></use></svg>
      Check
    </button>
    <button class="row-button is-primary" data-action="download" ${!isDownloadAllowed(game) ? "disabled" : ""} title="${escapeHtml(downloadTitle(game))}">
      <svg><use href="#icon-download"></use></svg>
      Download
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
  if (game.status === "blocked") return `<span class="pill blocked">${escapeHtml(blockedLabel(game))}</span>`;
  if (game.status === "queued") return `<span class="pill warn">In Queue</span>`;
  if (game.status === "available") return `<span class="pill ok">Update available</span>`;
  return `<span class="pill">Current</span>`;
}

function sourcePill(game) {
  const info = sourceInfo(game);
  return `<span class="pill ${info.className}">${escapeHtml(info.label)}</span>`;
}

function sourceInfo(game) {
  return SOURCE_TYPES[sourceType(game)] || SOURCE_TYPES.unknown;
}

function sourceType(game) {
  return game.source_type || "unknown";
}

function sourceDetail(game) {
  const src = sourceType(game);
  if (src === "shadowmount") return `Mount: ${game.mount_from || "unknown source"}`;
  if (src === "external") return `External: ${game.source_path || "external app metadata"}`;
  if (src === "official") return game.source_path || "Official installation";
  return "Source could not be identified";
}

function blockedLabel(game) {
  if (sourceType(game) === "unknown") return "Unknown source";
  if (!game.compatible_version) return "FW blocked";
  return "Blocked";
}

function renderQueue() {
  els.queueList.replaceChildren();
  if (!state.downloads.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "No active downloads.";
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

function renderSourcePolicies() {
  const rows = Object.keys(SOURCE_TYPES).map((type) => {
    const info = SOURCE_TYPES[type];
    const policy = sourcePolicyForType(type);
    const row = document.createElement("div");
    row.className = "policy-row";
    row.innerHTML = `
      <span class="pill ${info.className}">${escapeHtml(info.label)}</span>
      <strong>${policy.allow_install ? "Install allowed" : "Install blocked"}</strong>
      <em>${policy.allow_download ? "Download allowed" : "Download blocked"} - ${escapeHtml(info.description)}</em>
    `;
    return row;
  });
  els.sourcePolicyList.replaceChildren(...rows);
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
    showToast("Configuration saved.");
  } catch (error) {
    state.config = config;
    showToast("Configuration updated locally. API is not reachable yet.");
  }

  renderSettings();
}

async function runTitleAction(titleId, action) {
  const game = state.titles.find((item) => item.title_id === titleId);
  if (!game) return;

  if (action === "download" && !isDownloadAllowed(game)) {
    showToast(downloadTitle(game));
    return;
  }

  try {
    await postJson(API.action(titleId, action), {});
  } catch (error) {
    // Demo mode keeps the UI interactive before the ELF API exists.
  }

  if (action === "download" && game.compatible_version) {
    const detail = isInstallBlocked(game)
      ? "Download only - install blocked by source policy"
      : "Waiting to start";
    game.queued = true;
    game.status = "queued";
    if (!state.downloads.some((item) => item.title_id === titleId)) {
      state.downloads.push({
        title_id: game.title_id,
        name: game.name,
        version: game.compatible_version,
        progress: 0,
        detail,
      });
    }
    state.logs.push(`[${timeNow()}] Queued ${game.title_id} ${game.compatible_version} (${sourceType(game)}, install=${isInstallBlocked(game) ? "blocked" : "allowed"})`);
    showToast(`${game.title_id} was added to the queue.`);
  } else {
    state.logs.push(`[${timeNow()}] Check requested for ${game.title_id}`);
    showToast(`${game.title_id} check queued.`);
  }

  renderGames();
  renderQueue();
  renderLogs();
}

function updateGame(titleId, patch) {
  const game = state.titles.find((item) => item.title_id === titleId);
  if (!game) return;
  if (sourceType(game) === "unknown" && patch.mode && !["disabled", "check_only"].includes(patch.mode)) {
    patch.mode = "check_only";
    patch.enabled = true;
    showToast(`${titleId}: unknown source, check only is allowed.`);
  }
  Object.assign(game, patch);
  if (patch.mode === "disabled") game.enabled = false;
  state.logs.push(`[${timeNow()}] Policy updated for ${titleId}`);
  renderGames();
  renderLogs();
}

function isDownloadAllowed(game) {
  const policy = sourcePolicy(game);
  return Boolean(game.enabled && game.compatible_version && policy.allow_download);
}

function isInstallBlocked(game) {
  return !sourcePolicy(game).allow_install;
}

function sourcePolicy(game) {
  return sourcePolicyForType(sourceType(game));
}

function sourcePolicyForType(type) {
  const fallbackPolicy = fallback.config.source_policy[type] || fallback.config.source_policy.unknown;
  const configuredPolicy = (state.config.source_policy || {})[type] || {};
  return { ...fallbackPolicy, ...configuredPolicy };
}

function downloadTitle(game) {
  if (!game.enabled) return "Title is disabled.";
  if (!game.compatible_version) return "No compatible update version is available.";
  if (!sourcePolicy(game).allow_download) return "Downloads are blocked for this source.";
  if (isInstallBlocked(game)) return "Download allowed; installs stay blocked for this source.";
  return "Download compatible update.";
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
  return new Date().toLocaleTimeString("en-US", { hour12: false });
}

let toastTimer;
function showToast(message) {
  els.toast.textContent = message;
  els.toast.classList.add("is-visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => els.toast.classList.remove("is-visible"), 2600);
}
